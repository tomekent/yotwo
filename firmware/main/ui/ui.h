#pragma once
// The Yoto UI on the player: runs the UI logic (ui_core.c) in its own task, feeding it knob
// and button events and connecting it to ABSpresso's player and catalogue.
//
// The functions down to ui_show_message_locked() are the ones ABSpresso's shared code
// calls, with ABSpresso's signatures.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "abs_api.h"

// `events` is the queue from input_init() (NULL to run without controls).
void ui_init(QueueHandle_t events);
// When the UI task last drew a frame (esp_timer_get_time()), 0 if never: proof it is running.
int64_t ui_alive_us(void);

// The UI keeps using `books` until the next ui_set_books(); the caller frees the old array after.
void ui_set_books(const abs_book_t *books, int count);
void ui_set_source(bool from_cache);
void ui_set_libraries(const abs_library_t *libs, int count, const char *selected_id);
// Scrolls a message across the display once; NULL clears it. Takes the UI lock itself.
void ui_show_message_locked(const char *msg);

// The user picked another library from the menu: the main loop collects the request,
// loads that library, then calls ui_library_request_done() whether or not it worked.
bool ui_take_library_request(char *library_id, size_t len);
void ui_library_request_done(void);

// Start-up: a word of up to four characters (e.g. "WIFI", "SYNC"). Ignored once the
// library is showing.
void ui_show_status(const char *word, uint32_t rgb);
// Setup screen: the setup page's 4-digit code.
void ui_show_setup(const char *password);
// Up to four characters on the display for a while (the update code).
void ui_show_word(const char *text, int seconds);

/* For the player's web page (web/web.c) and the setup page. All safe from any task. */

// The player's name ("Tom's Yotwo"); its network address is made from it (web_hostname()).
const char *ui_player_name(void);
void ui_set_player_name(const char *name);
// The volume as the knob shows it, 0-100.
int ui_knob_volume(void);
// The name of the library on show ("" if none).
void ui_library_name(char *out, size_t len);
// A book's title and author by item id, if it is in the library on show.
bool ui_book_names(const char *item_id, char *title, size_t tlen, char *author, size_t alen);
