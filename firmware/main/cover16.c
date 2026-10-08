#include "cover16.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "storage.h"
#include "wifi.h"

static const char *TAG = "cover16";

#define FETCH_WIDTH  64         // asked of the server; 4 x 4 source pixels per LED
#define CACHE_SLOTS  320        // covers kept in memory (about 250 KB of PSRAM)
#define RETRY_US     (600 * 1000000LL)  // a cover that failed is unlikely to work a moment later
#define ID_LEN       40
#define NONE_MARK_VERSION 2     // bump when decoding changes, to retry covers marked unusable

typedef struct {
    char id[ID_LEN];
    uint32_t used;              // s_clock when last asked for; 0 = free
    uint8_t pixels[COVER16_BYTES];
} slot_t;

typedef enum { LOAD_OK, LOAD_NONE, LOAD_LATER } load_t;  // NONE: it has no usable cover

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static slot_t *s_cache;
static uint32_t s_clock;
static char s_want[ID_LEN];     // what the UI is waiting for; "" when dealt with
static char s_failed[ID_LEN];   // last wanted id that could not be loaded, and when
static int64_t s_failed_at;
static char (*s_queue)[ID_LEN]; // prefetch queue
static int s_queue_n, s_queue_next;
static int s_stat_sd, s_stat_net, s_stat_none;

static void cache_path(const char *id, char *path, size_t len)
{
    snprintf(path, len, COVER16_RGB_PATH, id);
}

// Call with the lock held.
static slot_t *find(const char *id)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].used && strcmp(id, s_cache[i].id) == 0) return &s_cache[i];
    }
    return NULL;
}

// Call with the lock held. `evict` false = only use a free slot (prefetching must not
// push out covers the user has actually looked at).
static bool remember(const char *id, const uint8_t *px, bool evict)
{
    slot_t *slot = NULL;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (!s_cache[i].used) { slot = &s_cache[i]; break; }
        if (evict && (!slot || s_cache[i].used < slot->used)) slot = &s_cache[i];
    }
    if (!slot) return false;
    memcpy(slot->pixels, px, COVER16_BYTES);
    strlcpy(slot->id, id, sizeof(slot->id));
    slot->used = ++s_clock;
    return true;
}

// Crops the decoded image to a centred square and box-averages it to 16 x 16.
static void reduce(const uint8_t *rgb, int w, int h, uint8_t out[COVER16_BYTES])
{
    static float lin[256];  // sRGB value -> linear light; the LEDs' brightness is linear in level
    if (lin[255] == 0) {
        for (int i = 0; i < 256; i++) lin[i] = powf(i / 255.0f, 2.2f);
    }
    const int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2;
    static float px[COVER16_BYTES];
    float peak = 0;
    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 16; cx++) {
            const int xa = x0 + cx * side / 16, xb = x0 + (cx + 1) * side / 16;
            const int ya = y0 + cy * side / 16, yb = y0 + (cy + 1) * side / 16;
            float sum[3] = { 0 };
            int n = 0;
            for (int y = ya; y < yb; y++) {
                for (int x = xa; x < xb; x++) {
                    const uint8_t *p = rgb + (y * w + x) * 3;
                    sum[0] += lin[p[0]]; sum[1] += lin[p[1]]; sum[2] += lin[p[2]];
                    n++;
                }
            }
            float *o = px + (cy * 16 + cx) * 3;
            // Averaging greys colours out, so push each pixel away from its own grey.
            const float r = sum[0] / (n ? n : 1), g = sum[1] / (n ? n : 1), b = sum[2] / (n ? n : 1);
            const float grey = 0.3f * r + 0.6f * g + 0.1f * b, sat = 1.35f;
            o[0] = fmaxf(0, grey + (r - grey) * sat);
            o[1] = fmaxf(0, grey + (g - grey) * sat);
            o[2] = fmaxf(0, grey + (b - grey) * sat);
            for (int k = 0; k < 3; k++) if (o[k] > peak) peak = o[k];
        }
    }
    // Stretch so the brightest channel is full scale (dark covers would otherwise vanish),
    // without amplifying a nearly black image into noise.
    const float gain = peak > 0.02f ? fminf(1.0f / peak, 4.0f) : 1.0f;
    for (int i = 0; i < COVER16_BYTES; i++) {
        const float v = px[i] * gain;
        out[i] = v >= 1.0f ? 255 : (uint8_t)(v * 255.0f + 0.5f);
    }
}

// The JPEG as it came from the server, kept for the player's web page (a browser can show
// it as it is, and may manage one our decoder cannot).
static void save_jpeg(const char *id, const uint8_t *jpg, size_t len)
{
    if (!storage_ready() || len < 100 || len > 64 * 1024) return;
    char path[96];
    snprintf(path, sizeof(path), COVER16_JPEG_PATH, id);
    storage_mkdirs(STORAGE_ROOT "/c64");
    storage_write_file(path, jpg, len);
}

static bool have_jpeg(const char *id)
{
    char path[96];
    snprintf(path, sizeof(path), COVER16_JPEG_PATH, id);
    return storage_file_size(path) >= 0;
}

// `out` NULL fetches and keeps the JPEG without decoding it.
static load_t fetch(const char *id, uint8_t out[COVER16_BYTES])
{
    uint8_t *jpg = NULL;
    size_t len = 0;
    if (abs_get_cover(id, FETCH_WIDTH, &jpg, &len) != ESP_OK) return LOAD_LATER;
    save_jpeg(id, jpg, len);
    if (!out) {  // only the JPEG was wanted
        free(jpg);
        return LOAD_OK;
    }
    load_t result = LOAD_NONE;
    esp_jpeg_image_cfg_t cfg = {
        .indata = jpg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) == ESP_OK && info.width >= 16 && info.height >= 16 &&
        info.width <= 512 && info.height <= 1024) {
        cfg.outbuf_size = (size_t)info.width * info.height * 3;
        cfg.outbuf = heap_caps_malloc(cfg.outbuf_size, MALLOC_CAP_SPIRAM);
        if (cfg.outbuf && esp_jpeg_decode(&cfg, &info) == ESP_OK) {
            reduce(cfg.outbuf, info.width, info.height, out);
            result = LOAD_OK;
        }
        free(cfg.outbuf);
    }
    if (result != LOAD_OK) {
        ESP_LOGW(TAG, "could not decode the cover of %s (%u bytes, begins %02X %02X %02X %02X)", id, (unsigned)len,
                 len > 0 ? jpg[0] : 0, len > 1 ? jpg[1] : 0, len > 2 ? jpg[2] : 0, len > 3 ? jpg[3] : 0);
    }
    free(jpg);
    return result;
}

// From the SD card if it is there, else from the server (and then onto the card).
// `spare_time`: this is the background pass over the library, which may also fetch the JPEG
// of a cover cached before JPEGs were kept.
static load_t load(const char *id, uint8_t px[COVER16_BYTES], bool spare_time)
{
    char path[96];
    cache_path(id, path, sizeof(path));
    if (storage_ready()) {
        size_t len = 0;
        char *cached = storage_read_file(path, &len);
        if (cached && len == COVER16_BYTES) {
            memcpy(px, cached, COVER16_BYTES);
            free(cached);
            s_stat_sd++;
            if (spare_time && wifi_is_connected() && !have_jpeg(id)) fetch(id, NULL);
            return LOAD_OK;
        }
        // "No usable cover" is recorded as two bytes, the second saying which decoder
        // gave up, so that a better decoder gets to try again.
        const bool marked_none = cached && len == 2 && cached[1] == NONE_MARK_VERSION;
        free(cached);
        // A cover this player could not decode may still be one a browser can show.
        if (marked_none && spare_time && wifi_is_connected() && !have_jpeg(id)) fetch(id, NULL);
        if (marked_none) return LOAD_NONE;
    }
    if (!wifi_is_connected()) return LOAD_LATER;
    const load_t r = fetch(id, px);
    if (r != LOAD_LATER && storage_ready()) {
        storage_mkdirs(STORAGE_ROOT "/c16");
        static const uint8_t none[2] = { 'N', NONE_MARK_VERSION };
        if (r == LOAD_OK) storage_write_file(path, px, COVER16_BYTES);
        else storage_write_file(path, none, sizeof(none));
    }
    if (r == LOAD_OK) s_stat_net++;
    if (r == LOAD_NONE) s_stat_none++;
    return r;
}

static void cover_task(void *arg)
{
    static uint8_t px[COVER16_BYTES];
    for (;;) {
        // The cover the UI is waiting for comes first, then the prefetch queue.
        char id[ID_LEN] = "";
        bool wanted = false, finished = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_want[0]) {
            strlcpy(id, s_want, sizeof(id));
            wanted = true;
        } else {
            while (s_queue_next < s_queue_n && !id[0]) {
                const char *next = s_queue[s_queue_next++];
                if (!find(next)) strlcpy(id, next, sizeof(id));
            }
            finished = !id[0] && s_queue_n > 0 && s_queue_next == s_queue_n;
            if (finished) s_queue_n = s_queue_next = 0;
        }
        xSemaphoreGive(s_lock);
        if (finished) {
            ESP_LOGI(TAG, "library covers ready: %d from the SD card, %d from the server, %d without a usable cover",
                     s_stat_sd, s_stat_net, s_stat_none);
        }
        if (!id[0]) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const load_t r = load(id, px, !wanted);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (r == LOAD_OK) remember(id, px, wanted);
        if (wanted) {
            if (r != LOAD_OK) {
                strlcpy(s_failed, id, sizeof(s_failed));
                s_failed_at = esp_timer_get_time();
            }
            if (strcmp(s_want, id) == 0) s_want[0] = 0;  // unless the UI has moved on meanwhile
        } else if (r == LOAD_LATER) {
            s_queue_n = s_queue_next = 0;  // offline: stop; the next library load queues it again
        }
        xSemaphoreGive(s_lock);
        if (!wanted) vTaskDelay(pdMS_TO_TICKS(20));  // leave room for playback
    }
}

void cover16_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cache = heap_caps_calloc(CACHE_SLOTS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    // On core 1, below the UI: core 0 already carries Wi-Fi, TLS, the library load and the
    // player's fetching, and was being held for 15 s at a time at start-up.
    // PSRAM stack, as ABSpresso's cover loader: the TLS handshake needs the room. Its SD
    // card calls all go through storage.c, which runs them on an internal stack (this task
    // crashed in the SD driver on 2026-10-08; see storage.h).
    xTaskCreatePinnedToCoreWithCaps(cover_task, "cover16", 8192, NULL, 1, &s_task, 1, MALLOC_CAP_SPIRAM);
}

bool cover16_get(const char *item_id, uint8_t out[COVER16_BYTES])
{
    if (!s_lock || !s_cache || !item_id || !item_id[0]) return false;
    bool have = false, ask = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *slot = find(item_id);
    if (slot) {
        memcpy(out, slot->pixels, COVER16_BYTES);
        slot->used = ++s_clock;
        have = true;
    } else if (strcmp(item_id, s_failed) == 0 && esp_timer_get_time() - s_failed_at < RETRY_US) {
        // tried recently; it has no cover or the server is out of reach
    } else if (strcmp(item_id, s_want) != 0) {
        strlcpy(s_want, item_id, sizeof(s_want));
        ask = true;
    }
    xSemaphoreGive(s_lock);
    if (ask) xTaskNotifyGive(s_task);
    return have;
}

void cover16_prefetch(const abs_book_t *books, int count)
{
    if (!s_lock || count <= 0) return;
    char (*queue)[ID_LEN] = heap_caps_malloc((size_t)count * ID_LEN, MALLOC_CAP_SPIRAM);
    if (!queue) return;
    for (int i = 0; i < count; i++) strlcpy(queue[i], books[i].id, ID_LEN);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_queue);
    s_queue = queue;
    s_queue_n = count;
    s_queue_next = 0;
    s_stat_sd = s_stat_net = s_stat_none = 0;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}
