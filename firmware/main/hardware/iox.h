#pragma once
// PI4IOE5V6416 16-bit IO expander on the I2C bus. It has no interrupt line to the ESP32 on
// this board, so inputs are polled.
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t iox_init(i2c_master_bus_handle_t bus);
// `pin` is an IOX_* value from pins.h. Safe from any task.
esp_err_t iox_set(int pin, bool level);
// All 16 inputs: port 0 in the low byte, port 1 in the high byte.
esp_err_t iox_read(uint16_t *inputs);
// One input from a value returned by iox_read().
static inline bool iox_bit(uint16_t inputs, int pin) { return (inputs >> pin) & 1; }
