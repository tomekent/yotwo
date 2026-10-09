#include "accel.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pins.h"

static const char *TAG = "accel";

#define REG_WHO_AM_I  0x0F  // 0x33
#define REG_CTRL1     0x20
#define REG_CTRL4     0x23
#define REG_OUT_Z_L   0x2C
#define AUTO_INC      0x80

#define FACE_DOWN_ENTER 12000  // the stock threshold for "on front"
#define FACE_DOWN_LEAVE 9000
#define ON_BACK         -15000
#define POLL_MS         200
#define HOLD_POLLS      5      // a second face-down before it counts
// Its stack. At 2560 (0.2.0 to 0.3.1) every start froze with the display waiting for an
// SPI bus nobody held, and starting without this task cured it. Most likely the stack
// overflowed (a log line on top of an I2C read) past FreeRTOS's end-of-stack check into
// the display's bus lock, allocated just before it. How much is used is logged, to confirm.
#define STACK_BYTES     4096

static i2c_master_dev_handle_t s_dev;
static volatile bool s_face_down;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

static esp_err_t read_z(int16_t *z)
{
    const uint8_t reg = REG_OUT_Z_L | AUTO_INC;
    uint8_t out[2];
    const esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, out, 2, 100);
    if (err == ESP_OK) *z = (int16_t)(out[0] | (out[1] << 8));
    return err;
}

static const char *pose_name(int16_t z)
{
    return z > FACE_DOWN_ENTER ? "on its front (face-down)" : z < ON_BACK ? "on its back" : "upright or on a side";
}

static void accel_task(void *arg)
{
    int held = 0, failures = 0, polls = 0;
    const char *last_pose = NULL;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (++polls == 50) {  // ten seconds in, with the start-up log lines behind it
            ESP_LOGI(TAG, "stack: %u of %d bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL), STACK_BYTES);
        }
        int16_t z;
        if (read_z(&z) != ESP_OK) {
            if (++failures == 10) ESP_LOGW(TAG, "accelerometer not answering");
            continue;
        }
        failures = 0;
        // Logged whenever the rough pose changes, with the reading: what this board's Z axis
        // actually does is to be confirmed on the player.
        const char *pose = pose_name(z);
        if (pose != last_pose) {
            ESP_LOGI(TAG, "%s (z=%d)", pose, z);
            last_pose = pose;
        }
        if (!s_face_down) {
            held = z > FACE_DOWN_ENTER ? held + 1 : 0;
            if (held >= HOLD_POLLS) {
                s_face_down = true;
                ESP_LOGI(TAG, "face-down: nightlight");
            }
        } else if (z < FACE_DOWN_LEAVE) {
            s_face_down = false;
            held = 0;
            ESP_LOGI(TAG, "picked up: nightlight off");
        }
    }
}

esp_err_t accel_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_ACCEL,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");
    uint8_t who = 0;
    const uint8_t reg = REG_WHO_AM_I;
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, &who, 1, 100);
    if (err != ESP_OK || who != 0x33) {
        ESP_LOGW(TAG, "no LIS2DH12 at 0x%02X (%s, id 0x%02X): no face-down nightlight", I2C_ADDR_ACCEL,
                 esp_err_to_name(err), who);
        return ESP_ERR_NOT_FOUND;
    }
    // 25 Hz, normal mode, X/Y/Z on; block data update, +-2 g.
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL1, 0x37), TAG, "ctrl1");
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL4, 0x80), TAG, "ctrl4");
    xTaskCreatePinnedToCore(accel_task, "accel", STACK_BYTES, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "LIS2DH12 ready");
    return ESP_OK;
}

bool accel_face_down(void)
{
    return s_face_down;
}
