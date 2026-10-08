#pragma once
// Cover art for the 16 x 16 display.
//
// The server resizes the cover to a small JPEG; a background task decodes it, crops it
// square and averages it down to 16 x 16, working in linear light and then lifting the
// saturation and levels so it reads on the LEDs.
//
// Two caches:
//  - the SD card: STORAGE_ROOT/c16/<item id>.rgb, 768 bytes each (2 bytes for a cover the
//    decoder cannot handle, so it is not downloaded again every time);
//  - memory: a few hundred covers, enough for a whole library of ordinary size.
//
// cover16_prefetch() fills both for every book of a library in the background, so that
// after the first time the covers are simply there, with or without the server.
#include <stdbool.h>
#include "storage.h"
#include <stdint.h>
#include "abs_api.h"

#define COVER16_BYTES (16 * 16 * 3)

void cover16_init(void);
// Copies the item's cover (row-major RGB, ready for the display) into `out` and returns
// true, or returns false and puts it at the front of the queue. Cheap to call often.
// Where a cover's files are on the SD card: the 16 x 16 pixels shown on the player (768
// bytes of RGB), and the small JPEG they were made from, kept for the player's web page.
#define COVER16_RGB_PATH  STORAGE_ROOT "/c16/%s.rgb"
#define COVER16_JPEG_PATH STORAGE_ROOT "/c64/%s.jpg"

bool cover16_get(const char *item_id, uint8_t out[COVER16_BYTES]);
// Queues every book's cover for loading (from the SD card, else the server). Replaces any
// earlier queue. The books are copied; the array need not outlive the call.
void cover16_prefetch(const abs_book_t *books, int count);
