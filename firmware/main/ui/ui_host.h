#pragma once
// What the UI logic (ui_core.c) needs from whatever it runs on. On the player this is
// implemented by ui.c on top of ABSpresso's player and catalogue; in the desktop simulator
// (tools/ui_sim) by a fake library. Keeping ui_core.c free of ESP-IDF lets it be tested
// without hardware.
#include <stdbool.h>

typedef enum {
    UI_EV_L_TURN,      // left knob, delta = detents (+ clockwise)
    UI_EV_L_PRESS,
    UI_EV_L_HOLD,
    UI_EV_R_TURN,      // right knob
    UI_EV_R_PRESS,
    UI_EV_R_HOLD,
    UI_EV_BOTH,        // both knobs pressed together
    UI_EV_POWER_HOLD,
    UI_EV_CARD_IN,     // a card was put in the slot
    UI_EV_CARD_OUT,    // ...and taken out
    UI_EV_POWER_PRESS, // the power button pressed briefly
    UI_EV_PLUGGED,     // external power connected
    UI_EV_UNPLUGGED,   // ...and disconnected
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    int delta;
} ui_event_t;

typedef enum {
    UI_PLAY_IDLE,
    UI_PLAY_LOADING,
    UI_PLAY_PLAYING,
    UI_PLAY_PAUSED,
    UI_PLAY_FINISHED,
    UI_PLAY_ERROR,
} ui_play_state_t;

typedef struct {
    ui_play_state_t state;
    const char *title;       // valid until the next call
    const char *chapter;     // "" if the book has no chapters
    int chapter_index;       // -1 if no chapters
    int chapter_count;
    float chapter_progress;  // 0-1 through the chapter (through the book if no chapters)
    int volume;              // 0-100
} ui_playback_t;

// Books of the selected library, in A-Z title order.
int host_book_count(void);
const char *host_book_title(int index);
const char *host_book_sort_title(int index);   // the title as filed (without a leading "The")
const char *host_book_author(int index);       // "" if unknown
const char *host_book_series(int index);       // e.g. "Discworld #3"; "" if not in a series
// Cover of a book as 16 x 16 RGB (768 bytes), or NULL if it has none or it is still being
// fetched (ask again). Valid until the next call.
const unsigned char *host_book_cover(int index);
double host_book_last_played(int index);       // timestamp, 0 if never played
double host_book_added(int index);             // timestamp
// The server's libraries. Selecting one reloads the books (ui_core_books_changed follows).
int host_library_count(void);
const char *host_library_name(int index);
int host_current_library(void);
void host_select_library(int index);
// The book being played, or the one to offer first (most recently listened to); -1 if none.
int host_current_book(void);
void host_play_book(int index);
void host_get_playback(ui_playback_t *out);
// Cover of the open book as 16 x 16 RGB (768 bytes, row-major), or NULL if there is none
// (yet). Valid until the next call.
const unsigned char *host_cover(void);
// Title of a chapter of the open book ("" if unknown); valid until the next call.
const char *host_chapter_title(int index);
void host_toggle_play(void);
void host_pause(void);
void host_set_volume(int volume);
void host_chapter_step(int delta);
// Whether lists and the player show cover art (true) or text only. Remembered.
bool host_cover_view(void);
void host_set_cover_view(bool on);
// The card in the slot, and what the player knows it as.
typedef enum { UI_CARD_NONE, UI_CARD_NEW, UI_CARD_BOOK, UI_CARD_SLEEP } ui_card_kind_t;
ui_card_kind_t host_card(void);
int host_card_minutes(void);            // a sleep card's timer length
// A book card: play its book (or carry on, if it is the one open and paused). The book
// may be in any library. Deferred until the server is in reach if need be.
void host_card_play(void);
// Whether what is open is the book of the card that was last in the slot.
bool host_card_book_open(void);
// Make the card in the slot a book card for `book` (an index, or -1 for the one open).
// False if there is nothing to link it to.
bool host_card_link(int book);
void host_card_make_sleep(int minutes);
void host_card_clear(void);
// Sleep mode: the display turned right down.
// Display brightness, 0-100 (the UI decides: day, night, idle, sleep).
void host_set_brightness(int percent);
// Running from the battery (no external power).
bool host_on_battery(void);
// 0 = fine (or on external power), 1 = nearly empty, 2 = empty: switch off.
int host_battery_alarm(void);
// The LED ring. host_set_ring() is given the final colour, 0xRRGGBB, brightness already
// applied (0 = off). The settings: UI_LIGHT_NIGHT 0/1 (the nightlight when face-down),
// UI_LIGHT_COLOUR 0xRRGGBB, UI_LIGHT_BRIGHT 10-100 %, UI_LIGHT_WAKE 0/1 (the wake light
// with the clock's sunrise), UI_LIGHT_HOLD minutes it stays on once the sun is up (5-15).
typedef enum { UI_LIGHT_NIGHT, UI_LIGHT_COLOUR, UI_LIGHT_BRIGHT, UI_LIGHT_WAKE, UI_LIGHT_HOLD } ui_light_setting_t;
void host_set_ring(unsigned rgb);
bool host_face_down(void);
int host_light_setting(ui_light_setting_t which);
void host_set_light_setting(ui_light_setting_t which, int value);
// The loudest the player may go, percent of its full output (10-100). host_set_volume() and
// the volume in ui_playback_t stay 0-100: that range is spread over 0 to the limit.
// Display brightness by day and by night, 10-100 %; between them it follows the clock's
// sunrise and sunset.
typedef enum { UI_BRIGHT_DAY, UI_BRIGHT_NIGHT } ui_bright_t;
int host_bright_setting(ui_bright_t which);
void host_set_bright_setting(ui_bright_t which, int percent);
int host_volume_limit(void);
void host_set_volume_limit(int percent);
// Minutes left alone on battery before the player switches itself off; 0 = never.
int host_auto_off(void);
void host_set_auto_off(int minutes);
// Minutes added when a finished sleep card is taken out and put back. Remembered.
int host_sleep_extend(void);
void host_set_sleep_extend(int minutes);
// Play if a book is open and paused.
void host_resume(void);

// The battery.
typedef struct {
    bool present;    // a sensible reading was had
    int percent;     // 0-100 from the voltage (reads high while charging)
    bool plugged;    // on external power, charging or not
} ui_battery_t;
void host_battery(ui_battery_t *out);

// Setup (Wi-Fi, server and sign-in) from a phone, started from the settings. Starting it
// returns the setup page's 4-digit code to show; saving restarts the player.
const char *host_setup_start(void);
// Leaves setup, unless a phone is in the middle of saving (false).
bool host_setup_stop(void);

// The clock. host_time() gives the local time as seconds since midnight, and false until
// the time is known (it comes from the network).
typedef enum {
    UI_CLOCK_24H,   // 0 = 12-hour, 1 = 24-hour
    UI_CLOCK_RISE,  // when the sun starts to rise, minutes after midnight
    UI_CLOCK_SET,   // when it has set
    UI_CLOCK_FADE,  // how long rising and setting take, minutes
} ui_clock_setting_t;
bool host_time(int *second_of_day);
int host_clock_setting(ui_clock_setting_t which);
void host_set_clock_setting(ui_clock_setting_t which, int value);
// The time zone as a POSIX TZ string (firmware only; chosen on the setup page).
const char *host_timezone(void);
void host_set_timezone(const char *tz);

// How fast long names scroll, in pixels a second. Remembered.
int host_scroll_speed(void);
void host_set_scroll_speed(int pixels_per_second);

// Readability mode: titles and chapter names are reworded to put the telling part first.
// Remembered.
bool host_readable(void);
void host_set_readable(bool on);
// Favourites: marked with both knobs, kept on the player (the server has no such thing).
bool host_book_favourite(int index);
// Whether the book has a complete copy on the SD card (and so plays without the server).
bool host_book_downloaded(int index);
// No Wi-Fi, and none for long enough that the player should offer only what it can play
// by itself. False again the moment Wi-Fi is back.
bool host_offline(void);
// The book's copy for the SD card, if one is wanted and not yet complete: how much of it has
// arrived, 0-100, and whether it is being fetched at this moment. -1 if there is nothing to
// show (no copy wanted, or the copy is complete).
int host_book_offline(int index, bool *fetching);
void host_toggle_favourite(int index);
void host_power_off(void);
