#pragma once
// The LED ring ("nightlight"): an RGB LED driven straight from three GPIOs by PWM
// (red 12, green 23, blue 19; config #01 "type": "MCU").
//
// GPIO 12 is also the flash-voltage strapping pin, sampled at power-on and at the reset
// kinds that re-latch strapping pins; at those the pin is an input with its pull-down, not
// driven by us, so lighting the ring does not affect booting. The stock firmware drives
// the same pin the same way.
#include <stdint.h>
#include "esp_err.h"

esp_err_t ring_init(void);
// 0-255 per channel, as perceived (a gamma curve is applied here). Cheap to call often.
void ring_set(uint8_t r, uint8_t g, uint8_t b);
