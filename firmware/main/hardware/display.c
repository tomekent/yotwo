#include "display.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "iox.h"
#include "pins.h"

static const char *TAG = "display";

#define CHIPS         4
#define RAM_BYTES     224  // 8 lines of 28 bytes; a line is 8 pixels x RGB plus 4 unused
#define LINE_STRIDE   28
#define LINE_BYTES    24

#define CMD_RAM_WRITE 0x80
#define CMD_RESET     0xCC

static const int s_cs[CHIPS] = { IOX_DISP_CS0, IOX_DISP_CS1, IOX_DISP_CS2, IOX_DISP_CS3 };
static spi_device_handle_t s_spi;
static uint8_t *s_tx;  // DMA-capable
// One user at a time: the UI task pushes frames while other tasks set the brightness
// (power_init() on the main task during start-up, the web page's settings). Without it the
// two shared s_tx and took the same SPI device's bus lock at once, which IDF cannot hand
// over: speckled frames, then the UI waiting for the bus for ever (the start-up freeze of
// 0.2.0 to 0.3.2; see the journal, 2026-10-09).
static SemaphoreHandle_t s_lock;

// One chip-select framed transfer: `head` bytes of command, then `len` bytes of data.
static esp_err_t xfer(int chip, const uint8_t *head, int head_len, const uint8_t *data, int len)
{
    memcpy(s_tx, head, head_len);
    if (len) memcpy(s_tx + head_len, data, len);
    spi_transaction_t t = { .length = (head_len + len) * 8, .tx_buffer = s_tx };
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_spi, portMAX_DELAY), TAG, "bus");
    iox_set(s_cs[chip], 0);
    const esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    iox_set(s_cs[chip], 1);
    spi_device_release_bus(s_spi);
    return err;
}

static esp_err_t cmd(int chip, uint8_t command, const uint8_t *data, int len)
{
    return xfer(chip, &command, 1, data, len);
}

static esp_err_t cmd1(int chip, uint8_t command, uint8_t value)
{
    return cmd(chip, command, &value, 1);
}

esp_err_t display_init(void)
{
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = 4000000,
        .mode = 0,
        .spics_io_num = -1,  // chip selects are on the IO expander
        .queue_size = 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "SPI device");
    s_tx = heap_caps_malloc(RAM_BYTES + 4, MALLOC_CAP_DMA);
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tx && s_lock, ESP_ERR_NO_MEM, TAG, "buffer");

    // The stock firmware's sequence: clear every chip's RAM, then per chip reset and configure.
    static const uint8_t zeros[RAM_BYTES];
    static const uint8_t ram_at_0[2] = { CMD_RAM_WRITE, 0x00 };
    static const uint8_t rows_used[4] = { 0x0F, 0xFF, 0xFF, 0x0F };
    for (int c = 0; c < CHIPS; c++) {
        ESP_RETURN_ON_ERROR(xfer(c, ram_at_0, 2, zeros, RAM_BYTES), TAG, "clear chip %d", c);
    }
    for (int c = 0; c < CHIPS; c++) {
        ESP_RETURN_ON_ERROR(cmd(c, CMD_RESET, NULL, 0), TAG, "reset chip %d", c);
    }
    vTaskDelay(pdMS_TO_TICKS(600));
    for (int c = 0; c < CHIPS; c++) {
        cmd1(c, 0x41, 0xFF);          // all 8 COM outputs
        cmd(c, 0x42, rows_used, 4);   // ROW outputs in use
        cmd1(c, 0x31, 0x00);          // greyscale mode
        cmd1(c, 0x32, 0x07);          // 8 COM lines
        cmd1(c, 0x36, 0x00);
        cmd1(c, 0x37, 20);            // global brightness, 0-63
        ESP_RETURN_ON_ERROR(cmd1(c, 0x35, 0x03), TAG, "enable chip %d", c);  // oscillator + display on
    }
    return ESP_OK;
}

esp_err_t display_push(const display_px_t frame[DISPLAY_H][DISPLAY_W])
{
    static const uint8_t ram_at_0[2] = { CMD_RAM_WRITE, 0x00 };
    // Only chips whose four rows changed are rewritten: each write costs two I2C
    // transactions for the chip select as well as the SPI transfer.
    static display_px_t shown[DISPLAY_H][DISPLAY_W];
    static bool shown_valid[CHIPS];
    uint8_t ram[RAM_BYTES];
    esp_err_t result = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int c = 0; c < CHIPS; c++) {
        const size_t rows_bytes = 4 * sizeof(frame[0]);
        if (shown_valid[c] && memcmp(shown[c * 4], frame[c * 4], rows_bytes) == 0) continue;
        memset(ram, 0, sizeof(ram));
        // Line 2n is the left half of the chip's nth row, line 2n+1 the right half.
        for (int line = 0; line < 8; line++) {
            const display_px_t *src = &frame[c * 4 + line / 2][(line & 1) * 8];
            uint8_t *dst = ram + line * LINE_STRIDE;
            for (int x = 0; x < 8; x++) {
                *dst++ = src[x].r >> 2;
                *dst++ = src[x].g >> 2;
                *dst++ = src[x].b >> 2;
            }
        }
        const esp_err_t err = xfer(c, ram_at_0, 2, ram, RAM_BYTES);
        if (err != ESP_OK) result = err;
        shown_valid[c] = err == ESP_OK;
        if (err == ESP_OK) memcpy(shown[c * 4], frame[c * 4], rows_bytes);
    }
    xSemaphoreGive(s_lock);
    return result;
}

void display_set_brightness(int percent)
{
    if (percent < 2) percent = 2;
    if (percent > 100) percent = 100;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int c = 0; c < CHIPS; c++) {
        cmd1(c, 0x37, percent * 63 / 100);
    }
    xSemaphoreGive(s_lock);
}
