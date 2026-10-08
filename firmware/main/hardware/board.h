#pragma once
// Yoto Player Gen 2 board support. The board_audio_* functions have the same signatures as
// ABSpresso's, which is what its player calls.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// I2C, the IO expander (which also latches the power on) and the shared SPI bus.
// Call first: on battery the player switches off when the power button is released
// unless the latch has been set.
esp_err_t board_init(void);
// The I2C bus (codec, IO expander, accelerometer), for drivers that add a device to it.
i2c_master_bus_handle_t board_i2c_bus(void);
// Releases the power latch. On battery this switches the player off; on USB it stays up.
void board_power_off(void);

bool board_power_button_pressed(void);
bool board_headphones_inserted(void);
// Call when the headphone socket's state changes: switches the speaker off or back on.
void board_headphones_changed(void);

// Brings up I2S and the ES8388 codec.
esp_err_t board_audio_init(void);
// Opens the output at the given format. Safe to call again when the format changes.
esp_err_t board_audio_open(int sample_rate, int channels, int bits);
void board_audio_close(void);
// Blocks until queued.
esp_err_t board_audio_write(int16_t *samples, int len);
// 0-100
void board_audio_set_volume(int volume);
