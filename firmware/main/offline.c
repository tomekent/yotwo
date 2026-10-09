#include "offline.h"
#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "abs_api.h"
#include "cards.h"
#include "download.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "favourites.h"
#include "iox.h"
#include "names.h"
#include "pins.h"
#include "player.h"
#include "portal.h"
#include "storage.h"
#include "wifi.h"

static const char *TAG = "offline";

#define ID_LEN     40
#define MAX_WANTED 200                    // more than 100 favourites and 64 cards together
#define CHECK_MS   5000
#define FREE_FLOOR (1024ULL * 1024 * 1024)  // no new download is begun with less than this free

typedef struct { char (*ids)[ID_LEN]; int count; } id_set_t;

static id_set_t s_keep;  // last pass: every favourite and every card's book
static SemaphoreHandle_t s_lock;  // guards s_keep and s_skip (the web server reads them)
static id_set_t s_skip;  // wanted, but told not to be kept
#define SKIP_FILE STORAGE_ROOT "/offline_skip.txt"
#define MAX_SKIP 64

// Books that have stopped being wanted, and for how many passes.
#define REMOVE_AFTER_PASSES (120000 / CHECK_MS)
static struct { char id[ID_LEN]; int passes; } s_doomed[32];
static int s_doomed_n;

static void doomed_add(const char *id)
{
    for (int i = 0; i < s_doomed_n; i++) {
        if (strcmp(s_doomed[i].id, id) == 0) return;
    }
    if (s_doomed_n >= 32) return;
    ESP_LOGI(TAG, "%.8s is no longer a favourite or on a card: its copy goes in two minutes unless that changes", id);
    strlcpy(s_doomed[s_doomed_n].id, id, ID_LEN);
    s_doomed[s_doomed_n++].passes = 0;
}

static bool in_set(const id_set_t *set, const char *id)
{
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->ids[i], id) == 0) return true;
    }
    return false;
}

static void add(id_set_t *set, char (*ids)[ID_LEN], int n)
{
    for (int i = 0; i < n && set->count < MAX_WANTED; i++) {
        if (ids[i][0] && !in_set(set, ids[i])) strlcpy(set->ids[set->count++], ids[i], ID_LEN);
    }
}

static void save_skips(void)  // with the lock held
{
    char *text = heap_caps_malloc((size_t)s_skip.count * ID_LEN + 1, MALLOC_CAP_SPIRAM);
    if (!text) return;
    size_t n = 0;
    for (int i = 0; i < s_skip.count; i++) n += (size_t)sprintf(text + n, "%s\n", s_skip.ids[i]);
    storage_write_file(SKIP_FILE, text, n);
    free(text);
}

static void load_skips(void)
{
    s_skip.ids = heap_caps_calloc(MAX_SKIP, ID_LEN, MALLOC_CAP_SPIRAM);
    char *text = storage_read_file(SKIP_FILE, NULL);
    for (char *line = text ? strtok(text, "\r\n") : NULL; line && s_skip.count < MAX_SKIP; line = strtok(NULL, "\r\n")) {
        if (line[0] && strlen(line) < ID_LEN) strlcpy(s_skip.ids[s_skip.count++], line, ID_LEN);
    }
    free(text);
}

bool offline_skipped(const char *item_id)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool skipped = in_set(&s_skip, item_id);
    xSemaphoreGive(s_lock);
    return skipped;
}

void offline_skip(const char *item_id, bool skip)
{
    if (!s_lock || !item_id[0] || strlen(item_id) >= ID_LEN) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was = in_set(&s_skip, item_id);
    if (skip && !was && s_skip.count < MAX_SKIP) {
        strlcpy(s_skip.ids[s_skip.count++], item_id, ID_LEN);
    } else if (!skip && was) {
        for (int i = 0; i < s_skip.count; i++) {
            if (strcmp(s_skip.ids[i], item_id) == 0) {
                memmove(s_skip.ids[i], s_skip.ids[i + 1], (size_t)(s_skip.count - i - 1) * ID_LEN);
                s_skip.count--;
                break;
            }
        }
    }
    if (skip != was) save_skips();
    xSemaphoreGive(s_lock);
    if (skip) {
        ESP_LOGI(TAG, "%.8s: not to be kept; removing its copy", item_id);
        download_remove(item_id);
    } else if (was) {
        ESP_LOGI(TAG, "%.8s: to be kept again", item_id);
    }
}

bool offline_is_wanted(const char *item_id)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool wanted = in_set(&s_keep, item_id);
    xSemaphoreGive(s_lock);
    return wanted;
}

int offline_wanted(char (*out)[40], int max)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_keep.count < max ? s_keep.count : max;
    memcpy(out, s_keep.ids, (size_t)n * ID_LEN);
    xSemaphoreGive(s_lock);
    return n;
}

offline_state_t offline_state(const char *item_id)
{
    switch (download_state(item_id, NULL)) {
    case DL_DONE:   return OFFLINE_YES;
    case DL_QUEUED:
    case DL_ACTIVE: return OFFLINE_PARTLY;
    default:        return OFFLINE_NO;
    }
}

static bool on_external_power(void)
{
    uint16_t pins;
    return iox_read(&pins) == ESP_OK && !iox_bit(pins, IOX_PLUG_STATUS);
}

static void offline_task(void *arg)
{
    char (*scratch)[ID_LEN] = heap_caps_malloc(MAX_WANTED * ID_LEN, MALLOC_CAP_SPIRAM);
    id_set_t keep = { heap_caps_malloc(MAX_WANTED * ID_LEN, MALLOC_CAP_SPIRAM), 0 };
    id_set_t fetch = { heap_caps_malloc(MAX_WANTED * ID_LEN, MALLOC_CAP_SPIRAM), 0 };
    s_keep.ids = heap_caps_malloc(MAX_WANTED * ID_LEN, MALLOC_CAP_SPIRAM);
    bool running = false, first = true;
    char reported[ID_LEN] = "";
    int64_t last_report = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(first ? 15000 : CHECK_MS));  // let start-up finish first

        // What should be on the card. To keep: every favourite, in whichever library, and
        // every card's book. To fetch: the same, less the favourites of libraries that are
        // not loaded (their kind is not known here, and a podcast show cannot be fetched).
        keep.count = fetch.count = 0;
        const int cards = cards_books(scratch, MAX_WANTED);
        add(&keep, scratch, cards);
        add(&fetch, scratch, cards);
        add(&keep, scratch, favourites_ids(scratch, MAX_WANTED, false));
        add(&fetch, scratch, favourites_ids(scratch, MAX_WANTED, true));

        // A book that was wanted on the last pass and is not now has been unmarked or had
        // its card cleared: its copy goes. (Nothing is removed on the first pass, when
        // there is no last pass to compare with.)
        // Not at once, though: only when it has stayed unwanted for two minutes. A list of
        // favourites that comes back wrong for a moment must not cost hours of downloading.
        for (int i = 0; !first && i < s_keep.count; i++) {
            if (!in_set(&keep, s_keep.ids[i]) && download_state(s_keep.ids[i], NULL) != DL_NONE) {
                doomed_add(s_keep.ids[i]);
            }
        }
        for (int i = s_doomed_n - 1; i >= 0; i--) {
            const bool wanted_again = in_set(&keep, s_doomed[i].id);
            if (!wanted_again && ++s_doomed[i].passes < REMOVE_AFTER_PASSES) continue;
            if (wanted_again) {
                ESP_LOGI(TAG, "%.8s is wanted again: its copy stays", s_doomed[i].id);
            } else {
                ESP_LOGI(TAG, "%.8s is no longer a favourite or on a card: removing its copy", s_doomed[i].id);
                download_remove(s_doomed[i].id);
            }
            s_doomed[i] = s_doomed[--s_doomed_n];
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_keep.ids, keep.ids, (size_t)keep.count * ID_LEN);
        s_keep.count = keep.count;
        // A skipped book that is no longer wanted at all needs no entry: should it become a
        // favourite again later, it is fetched like any other.
        bool dropped = false;
        for (int i = s_skip.count - 1; !first && i >= 0; i--) {
            if (in_set(&keep, s_skip.ids[i])) continue;
            memmove(s_skip.ids[i], s_skip.ids[i + 1], (size_t)(s_skip.count - i - 1) * ID_LEN);
            s_skip.count--;
            dropped = true;
        }
        if (dropped) save_skips();
        xSemaphoreGive(s_lock);
        first = false;
        names_work();  // a name for the web page's downloads list, if one is waiting

        player_status_t st;
        player_get_status(&st);
        const bool playing = st.state == PLAYER_LOADING || st.state == PLAYER_BUFFERING || st.state == PLAYER_PLAYING;
        const bool powered = on_external_power(), online = wifi_is_connected();
        // Not during setup either: it needs the sockets and the radio to itself.
        const bool allowed = storage_ready() && !playing && powered && online && !portal_active() && !ota_busy();
        if (allowed != running) {
            running = allowed;
            download_set_paused(!allowed);
            if (allowed) ESP_LOGI(TAG, "downloads may run (idle, on external power, online)");
            else ESP_LOGI(TAG, "downloads held:%s%s%s%s", playing ? " playing" : "", powered ? "" : " on battery",
                          online ? "" : " offline", portal_active() ? " setup" : "");
        }
        if (!allowed) continue;

        // One at a time: the next wanted book that has no copy, if there is room for it.
        if (!download_busy()) {
            for (int i = 0; i < fetch.count; i++) {
                if (download_state(fetch.ids[i], NULL) != DL_NONE || offline_skipped(fetch.ids[i])) continue;
                uint64_t free_b, total_b;
                storage_space(&free_b, &total_b);
                if (free_b < FREE_FLOOR) {
                    ESP_LOGW(TAG, "only %llu MB free on the SD card: no more downloads", free_b >> 20);
                    break;
                }
                abs_book_t book = { 0 };
                strlcpy(book.id, fetch.ids[i], sizeof(book.id));
                ESP_LOGI(TAG, "queueing %.8s (%llu MB free)", book.id, free_b >> 20);
                download_start(&book);
                break;
            }
        }

        // Progress, once a minute, for the log.
        const int64_t now = esp_timer_get_time();
        for (int i = 0; i < fetch.count && now - last_report > 60 * 1000000LL; i++) {
            int percent;
            if (download_state(fetch.ids[i], &percent) != DL_ACTIVE) continue;
            ESP_LOGI(TAG, "%.8s: %d%%%s", fetch.ids[i], percent, strcmp(reported, fetch.ids[i]) ? " (new)" : "");
            strlcpy(reported, fetch.ids[i], sizeof(reported));
            last_report = now;
            break;
        }
    }
}

void offline_init(void)
{
    if (!storage_ready()) {
        ESP_LOGW(TAG, "no SD card: nothing will be kept for offline");
        return;
    }
    download_set_paused(true);  // until the task has looked at the conditions
    s_lock = xSemaphoreCreateMutex();
    load_skips();
    // 8 KB: it also looks up names on the server (names_work()), and a TLS call needs room.
    xTaskCreatePinnedToCoreWithCaps(offline_task, "offline", 8192, NULL, 1, NULL, 1, MALLOC_CAP_SPIRAM);
}
