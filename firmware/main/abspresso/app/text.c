#include "text.h"

#include <stdint.h>

// Base letters for U+00C0..U+017F ('*' for the few that aren't letters).
static const char LATIN[] =
    // U+00C0..U+00FF: Latin-1 Supplement
    "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTs"
    "aaaaaaaceeeeiiiidnooooo*ouuuuyty"
    // U+0100..U+017F: Latin Extended-A
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGg"
    "GgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSs"
    "SsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

// Decodes one UTF-8 character and advances *p (malformed bytes are taken one at a time).
static uint32_t next_char(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    int len = 1;
    if (c >= 0xC0 && c < 0xE0 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        len = 2;
    } else if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        len = 3;
    } else if (c >= 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        len = 4;
    }
    *p += len;
    return c;
}

// The character with case and accents folded: Latin letters become lower-case ASCII.
static uint32_t fold(uint32_t c)
{
    if (c >= 0xC0 && c <= 0x17F && LATIN[c - 0xC0] != '*') c = (uint8_t)LATIN[c - 0xC0];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return c;
}

int text_cmp(const char *a, const char *b)
{
    for (;;) {
        if (!*a || !*b) return (uint8_t)*a - (uint8_t)*b;
        const uint32_t x = fold(next_char(&a)), y = fold(next_char(&b));
        if (x != y) return x < y ? -1 : 1;
    }
}

char text_initial(const char *s)
{
    if (!s || !*s) return '#';
    const uint32_t c = fold(next_char(&s));
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : '#';
}
