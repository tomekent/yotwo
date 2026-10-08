#include "board.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "iox.h"
#include "pins.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_codec;
static i2s_chan_handle_t s_i2s_tx;
static bool s_i2s_enabled;
static int s_rate, s_channels;
static int s_volume = 40;

esp_err_t board_init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "I2C bus");
    ESP_RETURN_ON_ERROR(iox_init(s_i2c), TAG, "IO expander");  // power latch is now held

    const spi_bus_config_t spi_cfg = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 720,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &spi_cfg, SPI_DMA_CH_AUTO), TAG, "SPI bus");
    return ESP_OK;
}

void board_power_off(void)
{
    iox_set(IOX_AMP_EN, 0);
    iox_set(IOX_VIN_HOLD, 0);
}

bool board_power_button_pressed(void)
{
    uint16_t in;
    return iox_read(&in) == ESP_OK && !iox_bit(in, IOX_POWER_BUTTON);
}

bool board_headphones_inserted(void)
{
    uint16_t in;
    return iox_read(&in) == ESP_OK && !iox_bit(in, IOX_HP_DETECT);
}

/* ES8388. Register values are the stock firmware's (docs/drivers.md), which is Espressif
   ADF's driver plus one extra write. The codec is an I2S slave with all four outputs on. */

static esp_err_t es_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_codec, buf, sizeof(buf), 100);
}

static esp_err_t es8388_init(void)
{
    static const uint8_t init[][2] = {
        { 0x19, 0x04 },  // mute DAC during setup
        { 0x01, 0x50 }, { 0x02, 0x00 },
        { 0x08, 0x00 },  // slave
        { 0x04, 0xC0 },  // outputs off for now
        { 0x00, 0x12 },
        { 0x17, 0x18 },  // 16-bit I2S
        { 0x18, 0x02 },  // MCLK / 256
        { 0x26, 0x00 }, { 0x27, 0x90 }, { 0x2A, 0x90 }, { 0x2B, 0x80 }, { 0x2D, 0x00 },
        { 0x1C, 0x08 },  // the stock firmware's addition
        { 0x1B, 0x00 }, { 0x1A, 0x00 },  // DAC digital volume 0 dB
        { 0x04, 0x3C },  // LOUT1, ROUT1, LOUT2, ROUT2 on
        { 0x03, 0xFF },  // ADC off
        { 0x09, 0xBB }, { 0x0A, 0x00 }, { 0x0B, 0x02 }, { 0x0C, 0x0D }, { 0x0D, 0x02 },
        { 0x10, 0xC0 }, { 0x11, 0xC0 },
        { 0x03, 0xFF },
    };
    for (int i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        ESP_RETURN_ON_ERROR(es_write(init[i][0], init[i][1]), TAG, "ES8388 reg 0x%02x", init[i][0]);
    }
    return ESP_OK;
}

static void apply_volume(void)
{
    // Output volume registers take 0-33; the two quietest steps lower the DAC instead.
    const int v = s_volume / 3;
    es_write(0x2E, v);
    es_write(0x2F, v);
    es_write(0x30, v);  // this unit is an "MP" board: both output pairs are used
    es_write(0x31, v);
    const int dac_att = (v > 0 || s_volume == 0) ? 0 : (s_volume == 1 ? 30 : 12);
    es_write(0x1B, dac_att);
    es_write(0x1A, dac_att);
}

static void set_amp(bool playing)
{
    iox_set(IOX_AMP_EN, playing && s_volume > 0 && !board_headphones_inserted());
}

// The speaker amplifier follows the headphone socket as it changes, not only when playback
// starts or the volume moves.
void board_headphones_changed(void)
{
    if (s_codec) set_amp(s_i2s_enabled);
}

esp_err_t board_audio_init(void)
{
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_ES8388,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_codec), TAG, "codec device");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    // About 185 ms of audio at 44.1 kHz stereo (the default is about 32 ms), so that a
    // short stall in the decoder does not reach the speaker as a gap.
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1023;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG, "I2S channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S std mode");
    ESP_RETURN_ON_ERROR(es8388_init(), TAG, "ES8388");
    apply_volume();
    return ESP_OK;
}

esp_err_t board_audio_open(int sample_rate, int channels, int bits)
{
    ESP_RETURN_ON_FALSE(s_i2s_tx && bits == 16 && (channels == 1 || channels == 2), ESP_ERR_INVALID_ARG, TAG,
                        "unsupported format");
    if (s_i2s_enabled && sample_rate == s_rate && channels == s_channels) {
        return ESP_OK;
    }
    board_audio_close();
    const i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    // The bus always runs stereo; mono streams are doubled up in board_audio_write().
    // (Letting the I2S driver handle mono went wrong on this chip: with the default slot
    // mask it was unclear both channels were fed, and with both slots selected a mono
    // book played at half speed.)
    const i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_i2s_tx, &clk), TAG, "clock");
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_slot(s_i2s_tx, &slot), TAG, "slot");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "enable");
    s_rate = sample_rate;
    s_channels = channels;
    s_i2s_enabled = true;
    es_write(0x19, 0x00);  // unmute
    set_amp(true);
    ESP_LOGI(TAG, "audio out %d Hz x%d", sample_rate, channels);
    return ESP_OK;
}

void board_audio_close(void)
{
    if (s_i2s_enabled) {
        set_amp(false);
        es_write(0x19, 0x04);
        i2s_channel_disable(s_i2s_tx);
        s_i2s_enabled = false;
    }
}

esp_err_t board_audio_write(int16_t *samples, int len)
{
    if (!s_i2s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t written;
    if (s_channels == 2) {
        return i2s_channel_write(s_i2s_tx, samples, len, &written, portMAX_DELAY);
    }
    // Mono: send each sample to both channels, a block at a time.
    static int16_t stereo[2 * 512];
    for (int n = len / 2; n > 0;) {
        const int block = n > 512 ? 512 : n;
        for (int i = 0; i < block; i++) {
            stereo[2 * i] = stereo[2 * i + 1] = samples[i];
        }
        const esp_err_t err = i2s_channel_write(s_i2s_tx, stereo, block * 4, &written, portMAX_DELAY);
        if (err != ESP_OK) return err;
        samples += block;
        n -= block;
    }
    return ESP_OK;
}

void board_audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    if (s_codec) {
        apply_volume();
        set_amp(s_i2s_enabled);
    }
}
