#pragma once
// Drawing on the 16 x 16 frame: the 3 x 5 pixel font and primitives from the UI design
// ("Yoto 16x16 Screen Mockups"). Four characters fit on a line, two lines on the display.
#include <stdint.h>
#include "display.h"

#define GFX_CHAR_W 4  // 3 pixels plus a 1 pixel gap

typedef display_px_t gfx_color_t;
#define GFX_RGB(hex) ((gfx_color_t){ ((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF })

// The design's palette.
#define GFX_WHITE  GFX_RGB(0xF4F1EA)
#define GFX_ORANGE GFX_RGB(0xFF8A3D)
#define GFX_GREEN  GFX_RGB(0x3DDC84)
#define GFX_YELLOW GFX_RGB(0xFFD166)
#define GFX_RED    GFX_RGB(0xFF4D4D)
#define GFX_VIOLET GFX_RGB(0x9B8CFF)
#define GFX_GREY   GFX_RGB(0x6A6A6A)
#define GFX_DIM    GFX_RGB(0x2C2C2C)

void gfx_clear(void);
void gfx_set(int x, int y, gfx_color_t c);
// Draws text with its top-left at (x, y); pixels off the display are clipped, so a negative
// x scrolls it. Lower case is drawn as upper case; unknown characters as spaces.
void gfx_text(const char *s, int x, int y, gfx_color_t c);
// Width in pixels of `s` (without the trailing gap).
// The same font with every pixel drawn as a scale x scale block.
void gfx_text_scaled(const char *s, int x, int y, gfx_color_t c, int scale);
int gfx_text_width(const char *s);
// Text with a black edge all round, so it reads over a picture.
void gfx_text_outlined(const char *s, int x, int y, gfx_color_t c);
// A 16 x 16 picture (768 bytes, row-major RGB) at `percent` of its brightness.
void gfx_image(const uint8_t *rgb, int percent);
void gfx_text_centered(const char *s, int y, gfx_color_t c);
// Sends the frame to the display.
void gfx_flush(void);
// The frame being drawn (for the simulator).
const display_px_t *gfx_frame(void);
