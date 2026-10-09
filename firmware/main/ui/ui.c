#include "ui.h"

#include <string.h>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_debug_helpers.h"
#include "battery.h"
#include "board.h"
#include "cards.h"
#include "display.h"
#include "download.h"
#include "nfc.h"
#include "power.h"
#include "wifi.h"
#include "catalog.h"
#include "favourites.h"
#include "config.h"
#include "nvs.h"
#include "cover16.h"
#include "gfx.h"
#include "player.h"
#include "offline.h"
#include "accel.h"
#include "portal.h"
#include "ring.h"
#include "web.h"
#include <time.h>
#include "ui_core.h"

static const char *TAG = "ui";

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_events;
static const abs_book_t *s_books;
static int s_book_count;
static player_status_t s_status;
#define MAX_LIBRARIES 12
static abs_library_t s_libs[MAX_LIBRARIES];
static int s_lib_count, s_lib_current = -1;
static char s_lib_request[40];
static volatile bool s_lib_requested;
static bool s_cover_view = true;
// The volume limit (see "Volume" below).
static int s_volume_limit = 80;  // percent of full output
static int s_knob_volume = -1;   // 0-100 as shown; -1 until worked out from the player's own

static int output_for(int knob) { return (knob * s_volume_limit + 50) / 100; }
// A pause asked for while the book is still being opened cannot be given yet (there is
// nothing to pause); it is remembered and given the moment the book starts.
static bool s_pause_when_ready;
static bool s_card_play_wanted;  // a card's book is to be played once the server can be reached
static bool play_card_now(void);
static bool s_readable;
static int s_scroll_speed = 12;  // pixels a second

// ABSpresso's shared code brackets its ui_* calls with these.
bool lvgl_port_lock(uint32_t timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lock, timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);
}

void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

/* ui_host.h, on top of ABSpresso. Called by ui_core from the UI task with the lock held. */

int host_book_count(void)
{
    return s_book_count;
}

const char *host_book_title(int index)
{
    return index >= 0 && index < s_book_count && s_books[index].title ? s_books[index].title : "";
}

const char *host_book_sort_title(int index)
{
    return index >= 0 && index < s_book_count && s_books[index].sort_title ? s_books[index].sort_title
                                                                            : host_book_title(index);
}

bool host_cover_view(void) { return s_cover_view; }

static void save_cover_view(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "covers", s_cover_view);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_battery(ui_battery_t *out)
{
    battery_status_t b;
    battery_get(&b);
    *out = (ui_battery_t){ .present = b.present, .percent = b.percent, .plugged = b.charging || b.charged };
    ESP_LOGI(TAG, "showing the battery: %d%%, %s", b.percent,
             b.charging ? "charging" : b.charged ? "on power, charged" : "on battery");
}

const char *host_setup_start(void)
{
    static char pass[16];
    char ssid[33];
    portal_start();  // takes port 80 from our page; the main loop restarts it afterwards
    portal_network(ssid, sizeof(ssid), pass, sizeof(pass));
    ESP_LOGI(TAG, "setup network '%s' (open), code %s", ssid, pass);
    return pass;
}

bool host_setup_stop(void)
{
    char msg[8];
    bool saving = false;
    portal_status(msg, sizeof(msg), &saving);
    if (saving) return false;  // a phone is saving the Wi-Fi: let it finish
    portal_stop();
    return true;
}

int host_scroll_speed(void) { return s_scroll_speed; }

static void save_scroll_speed(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "speed", (uint8_t)s_scroll_speed);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_set_scroll_speed(int pixels_per_second)
{
    if (pixels_per_second == s_scroll_speed || pixels_per_second < 1) return;
    s_scroll_speed = pixels_per_second;
    flash_safe(save_scroll_speed, NULL);
}

bool host_readable(void) { return s_readable; }

static void save_readable(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "readable", s_readable);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_set_readable(bool on)
{
    if (on == s_readable) return;
    s_readable = on;
    flash_safe(save_readable, NULL);
}

void host_set_cover_view(bool on)
{
    if (on == s_cover_view) return;
    s_cover_view = on;
    flash_safe(save_cover_view, NULL);
}

const char *host_book_author(int index)
{
    return index >= 0 && index < s_book_count && s_books[index].author ? s_books[index].author : "";
}

const char *host_book_series(int index)
{
    return index >= 0 && index < s_book_count && s_books[index].series ? s_books[index].series : "";
}

const unsigned char *host_book_cover(int index)
{
    static uint8_t cover[COVER16_BYTES];
    return index >= 0 && index < s_book_count && cover16_get(s_books[index].id, cover) ? cover : NULL;
}

double host_book_last_played(int index)
{
    return index >= 0 && index < s_book_count ? s_books[index].last_update : 0;
}

double host_book_added(int index)
{
    return index >= 0 && index < s_book_count ? s_books[index].added_at : 0;
}

int host_library_count(void) { return s_lib_count; }
const char *host_library_name(int index) { return index >= 0 && index < s_lib_count ? s_libs[index].name : ""; }
int host_current_library(void) { return s_lib_current; }

void host_select_library(int index)
{
    if (index < 0 || index >= s_lib_count) return;
    strlcpy(s_lib_request, s_libs[index].id, sizeof(s_lib_request));
    s_lib_requested = true;  // collected by the main loop, which does the loading
}

int host_current_book(void)
{
    int recent = -1;
    const bool offline = host_offline();
    for (int i = 0; i < s_book_count; i++) {
        if (s_status.state != PLAYER_IDLE && strcmp(s_books[i].id, s_status.item_id) == 0) return i;
        // (Offline, only a book with a copy on the card can be carried on with.)
        if (s_books[i].last_update > 0 && !s_books[i].finished &&
            (!offline || download_state(s_books[i].id, NULL) == DL_DONE) &&
            (recent < 0 || s_books[i].last_update > s_books[recent].last_update)) {
            recent = i;
        }
    }
    // Something is open that is not in this library (a card for a book in another one).
    if (s_status.state != PLAYER_IDLE) return -1;
    return recent;  // continue listening: the unfinished book played most recently
}

void host_play_book(int index)
{
    s_pause_when_ready = false;
    if (index >= 0 && index < s_book_count) player_open(&s_books[index]);
}

void host_get_playback(ui_playback_t *out)
{
    player_get_status(&s_status);
    if (s_card_play_wanted && wifi_is_connected()) {
        s_card_play_wanted = false;
        play_card_now();
    }
    if (s_pause_when_ready && s_status.state != PLAYER_LOADING) {
        s_pause_when_ready = false;
        if (s_status.state == PLAYER_PLAYING || s_status.state == PLAYER_BUFFERING) {
            player_toggle();
            player_get_status(&s_status);
        }
    }
    switch (s_status.state) {
    case PLAYER_LOADING:
    case PLAYER_BUFFERING: out->state = UI_PLAY_LOADING; break;
    case PLAYER_PLAYING:   out->state = UI_PLAY_PLAYING; break;
    case PLAYER_PAUSED:    out->state = UI_PLAY_PAUSED; break;
    case PLAYER_FINISHED:  out->state = UI_PLAY_FINISHED; break;
    case PLAYER_ERROR:     out->state = UI_PLAY_ERROR; break;
    default:               out->state = UI_PLAY_IDLE; break;
    }
    out->title = s_status.title;
    out->chapter = s_status.chapter;
    out->chapter_index = s_status.chapter_index;
    out->chapter_count = s_status.chapter_count;
    if (s_knob_volume < 0) {
        // First look: place the knob where the player's saved output falls on today's scale,
        // and bring the output down if it was saved louder than the limit now allows.
        s_knob_volume = s_status.volume * 100 / s_volume_limit;
        if (s_knob_volume > 100) s_knob_volume = 100;
        if (output_for(s_knob_volume) != s_status.volume) player_set_volume(output_for(s_knob_volume));
    }
    out->volume = s_knob_volume;
    const bool chapters = s_status.chapter_index >= 0 && s_status.chapter_end > s_status.chapter_start;
    const double start = chapters ? s_status.chapter_start : 0;
    const double length = chapters ? s_status.chapter_end - s_status.chapter_start : s_status.duration;
    const double p = length > 0 ? (s_status.position - start) / length : 0;
    out->chapter_progress = p < 0 ? 0 : p > 1 ? 1 : (float)p;
}

const char *host_chapter_title(int index)
{
    static char title[96];
    if (!player_get_chapter(index, title, sizeof(title), NULL, NULL)) title[0] = 0;
    return title;
}

const unsigned char *host_cover(void)
{
    static uint8_t cover[COVER16_BYTES];
    return s_status.state != PLAYER_IDLE && cover16_get(s_status.item_id, cover) ? cover : NULL;
}

void host_toggle_play(void) { s_pause_when_ready = false; player_toggle(); }

void host_pause(void)
{
    if (s_status.state == PLAYER_PLAYING || s_status.state == PLAYER_BUFFERING) player_toggle();
    else if (s_status.state == PLAYER_LOADING) s_pause_when_ready = true;
}

/* Volume. The knob and its display run 0-100; what reaches the amplifier is that share of
   the limit, so with the limit at 80 the top of the scale is 80 % of full output. */

/* The LED ring (hardware/ring.c) and the face-down sensor (hardware/accel.c). */

static int s_light[5] = { 1, 0xFF0000, 50, 0, 10 };  // nightlight on, red (the stock default), 50 %, wake off, 10 min
static const char *const LIGHT_KEY[5] = { "nlon", "nlrgb", "nlbr", "wake", "wakehold" };

void host_set_ring(unsigned rgb) { ring_set((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }
bool host_face_down(void) { return accel_face_down(); }
int host_light_setting(ui_light_setting_t which) { return s_light[which]; }

static void save_light(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < 5; i++) nvs_set_u32(h, LIGHT_KEY[i], (uint32_t)s_light[i]);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_set_light_setting(ui_light_setting_t which, int value)
{
    const bool ok = which == UI_LIGHT_COLOUR ? value >= 0 && value <= 0xFFFFFF
                  : which == UI_LIGHT_BRIGHT ? value >= 10 && value <= 100
                  : which == UI_LIGHT_HOLD   ? value >= 5 && value <= 15
                  : value == 0 || value == 1;
    if (!ok || value == s_light[which]) return;
    s_light[which] = value;
    flash_safe(save_light, NULL);
}

/* Display brightness by day and by night (ui_core blends between them with the clock). */

static int s_bright[2] = { 80, 40 };
static const char *const BRIGHT_KEY[2] = { "brday", "brnight" };

int host_bright_setting(ui_bright_t which) { return s_bright[which]; }

static void save_bright(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < 2; i++) nvs_set_u8(h, BRIGHT_KEY[i], (uint8_t)s_bright[i]);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_set_bright_setting(ui_bright_t which, int percent)
{
    if ((which != UI_BRIGHT_DAY && which != UI_BRIGHT_NIGHT) || percent < 10 || percent > 100 ||
        percent == s_bright[which]) {
        return;
    }
    s_bright[which] = percent;
    flash_safe(save_bright, NULL);
}

int host_volume_limit(void) { return s_volume_limit; }

static void save_volume_limit(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vollimit", (uint8_t)s_volume_limit);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_set_volume(int volume)
{
    s_knob_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    player_set_volume(output_for(s_knob_volume));
}

void host_set_volume_limit(int percent)
{
    if (percent < 10 || percent > 100 || percent == s_volume_limit) return;
    s_volume_limit = percent;
    flash_safe(save_volume_limit, NULL);
    if (s_knob_volume >= 0) player_set_volume(output_for(s_knob_volume));  // same place on the knob
}
void host_chapter_step(int delta)
{
    if (!delta) return;
    ESP_LOGI(TAG, "chapter %+d chosen (from %d of %d)", delta, s_status.chapter_index + 1, s_status.chapter_count);
    player_chapter_step(delta);
}

/* Cards. The reader says which card is in the slot; cards.c says what it means. */

static card_t s_card;           // the card last seen in the slot
static int s_sleep_extend = 5;

ui_card_kind_t host_card(void)
{
    char uid[16];
    nfc_uid_text(uid, sizeof(uid));
    if (!uid[0]) return UI_CARD_NONE;  // s_card keeps describing the card that was there
    if (!cards_find(uid, &s_card)) {
        memset(&s_card, 0, sizeof(s_card));
        strlcpy(s_card.uid, uid, sizeof(s_card.uid));
        return UI_CARD_NEW;
    }
    return s_card.kind == CARD_SLEEP ? UI_CARD_SLEEP : UI_CARD_BOOK;
}

int host_card_minutes(void) { return s_card.minutes; }

bool host_card_book_open(void)
{
    return s_card.kind == CARD_BOOK && s_status.state != PLAYER_IDLE && strcmp(s_card.item, s_status.item_id) == 0;
}

// Starts the card's book. A book needs only its id, title and author (the server supplies
// the position), so it need not be in the library on show. A podcast needs its show from
// the loaded library, which says which episode to carry on with.
static bool play_card_now(void)
{
    for (int i = 0; i < s_book_count; i++) {
        if (strcmp(s_books[i].id, s_card.item) == 0) {
            player_open(&s_books[i]);
            return true;
        }
    }
    if (s_card.podcast) {
        ESP_LOGW(TAG, "card %s is a podcast from a library that is not loaded", s_card.uid);
        return false;
    }
    abs_book_t book = { 0 };
    strlcpy(book.id, s_card.item, sizeof(book.id));
    book.title = s_card.title;
    book.author = s_card.author;
    player_open(&book);
    return true;
}

void host_card_play(void)
{
    if (s_card.kind != CARD_BOOK) return;
    s_pause_when_ready = false;
    if (host_card_book_open()) {
        host_resume();
        return;
    }
    ESP_LOGI(TAG, "card %s: %s", s_card.uid, s_card.title);
    // A downloaded book plays without the server; anything else waits for Wi-Fi.
    if (wifi_is_connected() || download_state(s_card.item, NULL) == DL_DONE) play_card_now();
    else s_card_play_wanted = true;
}

bool host_card_link(int book)
{
    card_t c = { 0 };
    strlcpy(c.uid, s_card.uid, sizeof(c.uid));
    c.kind = CARD_BOOK;
    if (!c.uid[0]) return false;
    if (book >= 0 && book < s_book_count) {
        strlcpy(c.item, s_books[book].id, sizeof(c.item));
        strlcpy(c.title, s_books[book].title ? s_books[book].title : "", sizeof(c.title));
        strlcpy(c.author, s_books[book].author ? s_books[book].author : "", sizeof(c.author));
        c.podcast = s_books[book].podcast;
    } else if (s_status.state != PLAYER_IDLE && s_status.item_id[0]) {
        strlcpy(c.item, s_status.item_id, sizeof(c.item));
        strlcpy(c.title, s_status.title, sizeof(c.title));
        strlcpy(c.author, s_status.author, sizeof(c.author));
        c.podcast = s_status.episode_id[0] != 0;
    } else {
        return false;
    }
    ESP_LOGI(TAG, "card %s linked to %s", c.uid, c.title);
    cards_put(&c);
    s_card = c;
    return true;
}

void host_card_make_sleep(int minutes)
{
    if (!s_card.uid[0]) return;
    card_t c = { 0 };
    strlcpy(c.uid, s_card.uid, sizeof(c.uid));
    c.kind = CARD_SLEEP;
    c.minutes = minutes;
    ESP_LOGI(TAG, "card %s is now a %d-minute sleep card", c.uid, minutes);
    cards_put(&c);
    s_card = c;
}

void host_card_clear(void)
{
    if (!s_card.uid[0]) return;
    ESP_LOGI(TAG, "card %s forgotten", s_card.uid);
    cards_remove(s_card.uid);
    s_card.kind = CARD_UNKNOWN;
}

void host_set_brightness(int percent)
{
    display_set_brightness(percent);
}

bool host_on_battery(void)
{
    battery_status_t b;
    battery_get(&b);
    return !b.charging && !b.charged;
}

// Nearly empty at 10 %; empty below 3.3 V, on three readings running (the voltage dips
// for a moment when the amplifier starts).
int host_battery_alarm(void)
{
    static int flat;
    battery_status_t b;
    battery_get(&b);
    if (!b.present || b.charging || b.charged) {
        flat = 0;
        return 0;
    }
    flat = b.volts < 3.30f ? flat + 1 : 0;
    if (flat >= 3) {
        ESP_LOGW(TAG, "battery empty (%.2f V): switching off", b.volts);
        return 2;
    }
    return b.percent <= 10 ? 1 : 0;
}

static int s_auto_off = 60;  // minutes; 0 = never

int host_auto_off(void) { return s_auto_off; }

static void save_auto_off(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "autooff", (uint16_t)s_auto_off);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_set_auto_off(int minutes)
{
    if (minutes == s_auto_off || minutes < 0 || minutes > 600) return;
    s_auto_off = minutes;
    flash_safe(save_auto_off, NULL);
}

/* The clock. The time comes from the network (main.c starts SNTP); these are its settings. */

#define DEFAULT_TZ "GMT0BST,M3.5.0/1,M10.5.0"  // United Kingdom
static int s_clock[4] = { 0, 7 * 60, 19 * 60, 15 };  // 12-hour; 7:00; 19:00; 15 minutes
static const char *const CLOCK_KEY[4] = { "clk24", "rise", "dusk", "fade" };
static char s_tz[48] = DEFAULT_TZ;

bool host_time(int *second_of_day)
{
    const time_t now = time(NULL);
    if (now < 1600000000) return false;  // not yet set from the network
    struct tm tm;
    localtime_r(&now, &tm);
    static bool told;
    if (!told) {
        told = true;
        ESP_LOGI(TAG, "time known: %02d:%02d local (zone %s)", tm.tm_hour, tm.tm_min, s_tz);
    }
    *second_of_day = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    return true;
}

int host_clock_setting(ui_clock_setting_t which) { return s_clock[which]; }

static void save_clock(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < 4; i++) nvs_set_u16(h, CLOCK_KEY[i], (uint16_t)s_clock[i]);
        nvs_set_str(h, "tz", s_tz);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_set_clock_setting(ui_clock_setting_t which, int value)
{
    if (value == s_clock[which] || value < 0 || value > 24 * 60) return;
    s_clock[which] = value;
    flash_safe(save_clock, NULL);
}

const char *host_timezone(void) { return s_tz; }

void host_set_timezone(const char *tz)
{
    if (!tz[0] || strlen(tz) >= sizeof(s_tz) || strcmp(tz, s_tz) == 0) return;
    strlcpy(s_tz, tz, sizeof(s_tz));
    setenv("TZ", s_tz, 1);
    tzset();
    flash_safe(save_clock, NULL);
}

int host_sleep_extend(void) { return s_sleep_extend; }

static void save_sleep_extend(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "extend", (uint8_t)s_sleep_extend);
        nvs_commit(h);
        nvs_close(h);
    }
}

void host_set_sleep_extend(int minutes)
{
    if (minutes == s_sleep_extend) return;
    s_sleep_extend = minutes;
    flash_safe(save_sleep_extend, NULL);
}

void host_resume(void)
{
    if (s_status.state == PLAYER_PAUSED) player_toggle();
}

/* Favourites live in favourites.c, which also mirrors them to the server. */

bool host_book_favourite(int index)
{
    return index >= 0 && index < s_book_count && favourites_has(s_books[index].id);
}

bool host_book_downloaded(int index)
{
    return index >= 0 && index < s_book_count && download_state(s_books[index].id, NULL) == DL_DONE;
}

// Offline once Wi-Fi has been gone for 20 s, which at start-up is several attempts to join.
#define OFFLINE_AFTER_US (20 * 1000000LL)

bool host_offline(void)
{
    static int64_t last_seen;  // 0 at start-up: counted from switching on
    const int64_t now = esp_timer_get_time();
    if (wifi_is_connected()) {
        last_seen = now;
        return false;
    }
    return now - last_seen >= OFFLINE_AFTER_US;
}

int host_book_offline(int index, bool *fetching)
{
    *fetching = false;
    if (index < 0 || index >= s_book_count) return -1;
    const char *id = s_books[index].id;
    int percent = 0;
    switch (download_state(id, &percent)) {
    case DL_DONE:
    case DL_REMOVING: return -1;
    case DL_ACTIVE:   *fetching = true; return percent;
    case DL_QUEUED:   return percent;  // waiting its turn, or held part-way
    default:          return offline_is_wanted(id) && !offline_skipped(id) ? 0 : -1;
    }
}

void host_toggle_favourite(int index)
{
    if (index >= 0 && index < s_book_count) favourites_toggle(s_books[index].id);
}

void host_power_off(void)
{
    ESP_LOGI(TAG, "power off");
    player_stop();  // closes the session, which saves the position on the server
    gfx_clear();
    gfx_text_centered("BYE", 5, GFX_WHITE);
    gfx_flush();
    vTaskDelay(pdMS_TO_TICKS(1500));
    gfx_clear();
    gfx_flush();
    board_power_off();
    // On battery the player is now off. On USB power it stays up: wait for the button to be
    // released and pressed again, then start afresh.
    vTaskDelay(pdMS_TO_TICKS(500));
    while (board_power_button_pressed()) vTaskDelay(pdMS_TO_TICKS(50));
    while (!board_power_button_pressed()) vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

/* For the web pages */

static char s_name[40] = "Yotwo";

const char *ui_player_name(void) { return s_name; }

static void save_name(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "name", s_name);
        nvs_commit(h);
        nvs_close(h);
    }
}

void ui_set_player_name(const char *name)
{
    while (*name == ' ') name++;
    if (!name[0] || strlen(name) >= sizeof(s_name) || strcmp(name, s_name) == 0) return;
    strlcpy(s_name, name, sizeof(s_name));
    flash_safe(save_name, NULL);
    web_name_changed();
}

int ui_knob_volume(void) { return s_knob_volume < 0 ? 0 : s_knob_volume; }

// These two are called from the web server's task. They give up rather than wait long for
// the UI's lock: the UI task holds it while it stops that server (to start setup), and a
// request waiting on it then would never let the server stop.
void ui_library_name(char *out, size_t len)
{
    out[0] = 0;
    if (!lvgl_port_lock(300)) return;
    strlcpy(out, s_lib_current >= 0 && s_lib_current < s_lib_count ? s_libs[s_lib_current].name : "", len);
    lvgl_port_unlock();
}

bool ui_book_names(const char *item_id, char *title, size_t tlen, char *author, size_t alen)
{
    bool found = false;
    if (!lvgl_port_lock(300)) return false;
    for (int i = 0; i < s_book_count && !found; i++) {
        if (strcmp(s_books[i].id, item_id) != 0) continue;
        strlcpy(title, s_books[i].title ? s_books[i].title : "", tlen);
        strlcpy(author, s_books[i].author ? s_books[i].author : "", alen);
        found = true;
    }
    lvgl_port_unlock();
    return found;
}

/* The UI task */

static volatile int64_t s_alive_us;  // when the task last drew a frame

int64_t ui_alive_us(void)
{
    return s_alive_us;
}

// A UI that has stopped drawing for this long is stuck: say who holds its lock and where
// every task is, once per stall. (The start-up freeze of 0.2.x: the UI task and the main
// loop both stopped, without a crash or a watchdog message.) The addresses decode with
// xtensa-esp32-elf-addr2line -pfiaC -e build/yotwo.elf.
#define STALL_US (8 * 1000000LL)

static void stall_check(void *arg)
{
    static bool told;
    const int64_t age = esp_timer_get_time() - s_alive_us;
    if (!s_alive_us || age < STALL_US) {
        told = false;
        return;
    }
    if (told) return;
    told = true;
    TaskHandle_t holder = xSemaphoreGetMutexHolder(s_lock);
    ESP_LOGE(TAG, "the UI has not drawn for %d s; its lock is held by %s", (int)(age / 1000000),
             holder ? pcTaskGetName(holder) : "nobody");
    esp_backtrace_print_all_tasks(20);
}

static void ui_task(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        // Handle input as it arrives, drawing a frame every UI_FRAME_MS.
        ui_event_t ev;
        const TickType_t now = xTaskGetTickCount();
        const TickType_t wait = (int32_t)(next - now) > 0 ? next - now : 0;
        if (s_events && xQueueReceive(s_events, &ev, wait) == pdTRUE) {
            lvgl_port_lock(0);
            ui_core_event(&ev);
            lvgl_port_unlock();
            continue;
        }
        if (!s_events) vTaskDelay(wait);
        next += pdMS_TO_TICKS(UI_FRAME_MS);
        lvgl_port_lock(0);
        ui_core_tick();
        gfx_flush();
        lvgl_port_unlock();
        s_alive_us = esp_timer_get_time();
    }
}

void ui_init(QueueHandle_t events)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_events = events;
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        uint8_t covers = 1;
        nvs_get_u8(h, "covers", &covers);
        s_cover_view = covers != 0;
        uint8_t readable = 0, extend = 5;
        nvs_get_u8(h, "readable", &readable);
        s_readable = readable != 0;
        nvs_get_u8(h, "extend", &extend);
        uint8_t speed = 12;
        nvs_get_u8(h, "speed", &speed);
        s_scroll_speed = speed ? speed : 12;
        if (s_scroll_speed > 18) s_scroll_speed = 18;  // a faster setting that has since been removed
        s_sleep_extend = extend ? extend : 5;
        for (int i = 0; i < 4; i++) {
            uint16_t v;
            if (nvs_get_u16(h, CLOCK_KEY[i], &v) == ESP_OK) s_clock[i] = v;
        }
        uint8_t limit;
        if (nvs_get_u8(h, "vollimit", &limit) == ESP_OK && limit >= 10 && limit <= 100) s_volume_limit = limit;
        for (int i = 0; i < 2; i++) {
            uint8_t b;
            if (nvs_get_u8(h, BRIGHT_KEY[i], &b) == ESP_OK && b >= 10 && b <= 100) s_bright[i] = b;
        }
        for (int i = 0; i < 5; i++) {
            uint32_t v;
            if (nvs_get_u32(h, LIGHT_KEY[i], &v) == ESP_OK) s_light[i] = (int)v;
        }
        uint16_t auto_off;
        if (nvs_get_u16(h, "autooff", &auto_off) == ESP_OK) s_auto_off = auto_off;
        size_t name_len = sizeof(s_name);
        if (nvs_get_str(h, "name", s_name, &name_len) != ESP_OK || !s_name[0]) strlcpy(s_name, "Yotwo", sizeof(s_name));
        size_t n = sizeof(s_tz);
        if (nvs_get_str(h, "tz", s_tz, &n) != ESP_OK || !s_tz[0]) strlcpy(s_tz, DEFAULT_TZ, sizeof(s_tz));
        nvs_close(h);
    }
    setenv("TZ", s_tz, 1);
    tzset();
    ui_core_init();
    xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 3, NULL, 1);
    static esp_timer_handle_t stall;
    const esp_timer_create_args_t args = { .callback = stall_check, .name = "ui stall" };
    if (esp_timer_create(&args, &stall) == ESP_OK) esp_timer_start_periodic(stall, 2000000);
}

/* Called by ABSpresso's shared code (which holds the lock) and by main */

void ui_set_books(const abs_book_t *books, int count)
{
    lvgl_port_lock(0);
    s_books = books;
    s_book_count = count;
    ui_core_books_changed();
    cover16_prefetch(books, count);  // every cover of this library, in the background
    favourites_set_library(catalog_library_id(), catalog_library_is_podcast(), books, count);
    lvgl_port_unlock();
}

void ui_set_source(bool from_cache)
{
    (void)from_cache;
}

void ui_set_libraries(const abs_library_t *libs, int count, const char *selected_id)
{
    lvgl_port_lock(0);
    s_lib_count = count > MAX_LIBRARIES ? MAX_LIBRARIES : count;
    s_lib_current = -1;
    for (int i = 0; i < s_lib_count; i++) {
        s_libs[i] = libs[i];
        if (selected_id && strcmp(libs[i].id, selected_id) == 0) s_lib_current = i;
    }
    lvgl_port_unlock();
}

bool ui_take_library_request(char *library_id, size_t len)
{
    if (!s_lib_requested) return false;
    lvgl_port_lock(0);
    strlcpy(library_id, s_lib_request, len);
    s_lib_requested = false;
    lvgl_port_unlock();
    return true;
}

void ui_library_request_done(void)
{
    lvgl_port_lock(0);
    ui_core_books_changed();
    lvgl_port_unlock();
}

void ui_show_message_locked(const char *msg)
{
    lvgl_port_lock(0);
    ui_core_message(msg);
    lvgl_port_unlock();
}

void ui_show_status(const char *word, uint32_t rgb)
{
    lvgl_port_lock(0);
    ui_core_status(word, rgb);
    lvgl_port_unlock();
}

void ui_show_setup(const char *password)
{
    lvgl_port_lock(0);
    ui_core_setup(password);
    lvgl_port_unlock();
}

void ui_show_word(const char *text, int seconds)
{
    lvgl_port_lock(0);
    ui_core_word(text, (unsigned)seconds);
    lvgl_port_unlock();
}
