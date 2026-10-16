#pragma once
// 16 x 16 RGB LED matrix: four HT16D35x drivers on the shared SPI bus, each driving four
// rows of sixteen pixels. See docs/hardware.md.
#include <stdint.h>
#include "esp_err.h"

#define DISPLAY_W 16
#define DISPLAY_H 16

// One pixel: red, green, blue, each 0-255 (scaled to the drivers' 6-bit levels on push).
typedef struct { uint8_t r, g, b; } display_px_t;

// Needs board_init() first.
esp_err_t display_init(void);
// Sends a whole frame, row-major from the top-left.
esp_err_t display_push(const display_px_t frame[DISPLAY_H][DISPLAY_W]);
// 0-100
void display_set_brightness(int percent);
