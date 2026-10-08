#pragma once
// The LIS2DH12 accelerometer, for telling when the player has been laid on its face.
//
// As the stock firmware: only the Z axis is read (raw, 16384 per g at +-2 g). Above
// +12000 (about 0.73 g) the player is "on its front", i.e. face-down; the stock code calls
// below -15000 "on its back". Face-down here also needs that to hold for a second, and ends
// when Z falls back under +9000, so a wobble does not flick the nightlight on and off.
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t accel_init(i2c_master_bus_handle_t bus);
bool accel_face_down(void);
