#include "gfx.h"

#include <string.h>

static display_px_t s_frame[DISPLAY_H][DISPLAY_W];

// 3 x 5 glyphs, one byte per row, bit 2 = left pixel.
typedef struct { char ch; uint8_t rows[5]; } glyph_t;
static const glyph_t s_font[] = {
    {'A',{2,5,7,5,5}}, {'B',{6,5,6,5,6}}, {'C',{3,4,4,4,3}}, {'D',{6,5,5,5,6}}, {'E',{7,4,6,4,7}},
    {'F',{7,4,6,4,4}}, {'G',{3,4,5,5,3}}, {'H',{5,5,7,5,5}}, {'I',{7,2,2,2,7}}, {'J',{1,1,1,5,2}},
    {'K',{5,5,6,5,5}}, {'L',{4,4,4,4,7}}, {'M',{5,7,7,5,5}}, {'N',{6,5,5,5,5}}, {'O',{2,5,5,5,2}},
    {'P',{6,5,6,4,4}}, {'Q',{2,5,5,6,3}}, {'R',{6,5,6,5,5}}, {'S',{3,4,2,1,6}}, {'T',{7,2,2,2,2}},
    {'U',{5,5,5,5,7}}, {'V',{5,5,5,5,2}}, {'W',{5,5,7,7,5}}, {'X',{5,5,2,5,5}}, {'Y',{5,5,2,2,2}},
    {'Z',{7,1,2,4,7}},
    {'0',{7,5,5,5,7}}, {'1',{2,6,2,2,7}}, {'2',{6,1,2,4,7}}, {'3',{6,1,2,1,6}}, {'4',{5,5,7,1,1}},
    {'5',{7,4,6,1,6}}, {'6',{3,4,7,5,7}}, {'7',{7,1,2,2,2}}, {'8',{7,5,7,5,7}}, {'9',{7,5,7,1,6}},
    {':',{0,2,0,2,0}}, {'>',{4,6,7,6,4}}, {'/',{1,1,2,4,4}}, {'-',{0,0,7,0,0}}, {'.',{0,0,0,0,2}},
    {'!',{2,2,2,0,2}}, {'?',{6,1,2,0,2}}, {'\'',{2,2,0,0,0}}, {',',{0,0,0,2,4}},
    {'#',{5,7,5,7,5}}, {'&',{2,5,2,5,3}}, {'+',{0,2,7,2,0}},
};

static const uint8_t *glyph(char ch)
{
    static const uint8_t blank[5];
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    for (int i = 0; i < (int)(sizeof(s_font) / sizeof(s_font[0])); i++) {
        if (s_font[i].ch == ch) return s_font[i].rows;
    }
    return blank;
}

void gfx_clear(void)
{
    memset(s_frame, 0, sizeof(s_frame));
}

void gfx_set(int x, int y, gfx_color_t c)
{
    if (x >= 0 && x < DISPLAY_W && y >= 0 && y < DISPLAY_H) s_frame[y][x] = c;
}

void gfx_text(const char *s, int x, int y, gfx_color_t c)
{
    for (; *s; s++, x += GFX_CHAR_W) {
        if (x <= -GFX_CHAR_W || x >= DISPLAY_W) continue;
        const uint8_t *g = glyph(*s);
        for (int r = 0; r < 5; r++) {
            for (int k = 0; k < 3; k++) {
                if (g[r] & (4 >> k)) gfx_set(x + k, y + r, c);
            }
        }
    }
}

void gfx_text_scaled(const char *s, int x, int y, gfx_color_t c, int scale)
{
    for (; *s; s++, x += GFX_CHAR_W * scale) {
        const uint8_t *g = glyph(*s);
        for (int r = 0; r < 5; r++) {
            for (int k = 0; k < 3; k++) {
                if (!(g[r] & (4 >> k))) continue;
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) gfx_set(x + k * scale + dx, y + r * scale + dy, c);
                }
            }
        }
    }
}

void gfx_text_outlined(const char *s, int x, int y, gfx_color_t c)
{
    const gfx_color_t black = { 0, 0, 0 };
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx || dy) gfx_text(s, x + dx, y + dy, black);
        }
    }
    gfx_text(s, x, y, c);
}

void gfx_image(const uint8_t *rgb, int percent)
{
    for (int i = 0; i < DISPLAY_W * DISPLAY_H; i++, rgb += 3) {
        s_frame[i / DISPLAY_W][i % DISPLAY_W] =
            (display_px_t){ rgb[0] * percent / 100, rgb[1] * percent / 100, rgb[2] * percent / 100 };
    }
}

int gfx_text_width(const char *s)
{
    const int n = strlen(s);
    return n ? n * GFX_CHAR_W - 1 : 0;
}

void gfx_text_centered(const char *s, int y, gfx_color_t c)
{
    gfx_text(s, (DISPLAY_W - gfx_text_width(s)) / 2, y, c);
}

void gfx_flush(void)
{
    display_push(s_frame);
}

const display_px_t *gfx_frame(void)
{
    return &s_frame[0][0];
}
