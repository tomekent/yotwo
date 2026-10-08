#pragma once
// Simulator stand-in for firmware/main/hardware/display.h: same types, no hardware.
#include <stdint.h>

#define DISPLAY_W 16
#define DISPLAY_H 16

typedef struct { uint8_t r, g, b; } display_px_t;

static inline int display_push(const display_px_t frame[DISPLAY_H][DISPLAY_W]) { (void)frame; return 0; }
