#pragma once
// The two-knob UI from the design ("Yoto 16x16 Screen Mockups", Navigator artboard):
//
//   Left knob  = sound, everywhere: turn for volume, press for play/pause.
//   Right knob = where you are: in the player, turn offers another chapter (press to go
//                to it; left alone it goes back) and press opens the menu; in menus, turn
//                moves, press selects and hold goes back.
//   Both knobs = mark or unmark the book as a favourite; with an unlinked card in the
//                slot, link the card to the book instead.
//   Cards      = a book card plays its book and pauses it when taken out; a sleep card
//                dims the display and runs a sleep timer while it is in.
//   Menu       = one list: LAST (recently played), NEW (recently added), FAV (favourites),
//                A-Z (the alphabet, then titles from that letter), AUTH (authors, then their
//                books), SERI (series, then their books in order), BED (sleep timer), SET
//                (settings: LIB library, VIEW cover art or text, READ readability mode,
//                SPD scroll speed, CARD the card in the slot, EXT sleep extension, CLK the clock, AUTO switching off, VOL the volume limit, LITE the LED ring, WIFI the setup network (Wi-Fi only; the server and sign-in are on the web page)
//                Wi-Fi and sign-in from a phone).
//   Lists      = all drawn alike: a picture or icon on top, the name along the bottom,
//                a position line on the bottom row. They wrap round at both ends.
//
// Pure logic and drawing (through gfx.h); no ESP-IDF. All calls must come from one task.
#include "ui_host.h"

#define UI_FRAME_MS 40  // one tick. Short enough that the fastest scroll is a pixel at a time.

void ui_core_init(void);
void ui_core_event(const ui_event_t *ev);
// Advances one frame and draws it (the caller then flushes).
void ui_core_tick(void);

// Start-up and system screens.
void ui_core_status(const char *word, unsigned rgb);  // up to 4 characters; NULL clears
void ui_core_setup(const char *password);             // the setup page's 4-digit code
void ui_core_message(const char *msg);
// Up to four characters in the middle of the display for `seconds` (any knob still works
// underneath; another overlay replaces it).
void ui_core_display_test(void);                                     // red, green, blue, white, then a row and column sweep (about 20 s)
void ui_core_word(const char *text, unsigned seconds);                // scrolls once over anything; NULL clears
// The books were (re)loaded, or a library switch finished (successfully or not).
void ui_core_books_changed(void);
