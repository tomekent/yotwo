#include "iox.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pins.h"

static const char *TAG = "iox";

#define REG_INPUT  0x00
#define REG_OUTPUT 0x02
#define REG_CONFIG 0x06  // 1 = input
#define REG_PULL_ENABLE 0x46
#define REG_PULL_SELECT 0x48

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;
static uint8_t s_out[2] = { IOX_OUT_P0, IOX_OUT_P1 };

static esp_err_t write2(uint8_t reg, uint8_t p0, uint8_t p1)
{
    const uint8_t buf[3] = { reg, p0, p1 };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

esp_err_t iox_init(i2c_master_bus_handle_t bus)
{
    s_lock = xSemaphoreCreateMutex();
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_IOX,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");
    // Output latches first, so pins take their safe levels the moment they become outputs
    // (in particular the power latch goes high).
    ESP_RETURN_ON_ERROR(write2(REG_OUTPUT, s_out[0], s_out[1]), TAG, "outputs");
    ESP_RETURN_ON_ERROR(write2(REG_CONFIG, IOX_DIR_P0, IOX_DIR_P1), TAG, "directions");
    // Pull-ups on the button and charger-status inputs. Without them the knob buttons
    // float and read as random presses. The stock firmware writes these and, on this
    // board, carries on if the writes fail, so do the same.
    const esp_err_t e1 = write2(REG_PULL_ENABLE, IOX_PULL_P0, IOX_PULL_P1);
    const esp_err_t e2 = write2(REG_PULL_SELECT, IOX_PULL_P0, IOX_PULL_P1);  // 1 = pull up
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGW(TAG, "pull-up registers not accepted (%s, %s)", esp_err_to_name(e1), esp_err_to_name(e2));
    }
    return ESP_OK;
}

esp_err_t iox_set(int pin, bool level)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int port = pin >> 3;
    const uint8_t mask = 1 << (pin & 7);
    s_out[port] = level ? (s_out[port] | mask) : (s_out[port] & ~mask);
    const uint8_t buf[2] = { REG_OUTPUT + port, s_out[port] };
    const esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t iox_read(uint16_t *inputs)
{
    const uint8_t reg = REG_INPUT;
    uint8_t in[2] = { 0xFF, 0xFF };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, in, 2, 100);
    xSemaphoreGive(s_lock);
    *inputs = in[0] | (in[1] << 8);
    return err;
}
