#include "download.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "storage.h"
#include "wifi.h"

static const char *TAG = "download";

#define DL_ROOT          STORAGE_ROOT "/dl"
#define SEGMENT_SECONDS  6.0
#define MAX_DOWNLOADS    64
#define SYNC_EVERY       10  // segments between fsyncs (index entries are written only after a sync)
#define SEGMENT_RETRIES  40
#define MAX_FAILURES     3   // whole-run failures before a download is marked as errored

typedef struct {
    char id[40];
    dl_state_t state;
    int done;   // completed segments
    int total;  // 0 until the session has been started once
    int failures;
} entry_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_paused;  // (yotwo) downloads held: stop at once, keep what is done, resume later
static entry_t s_entries[MAX_DOWNLOADS];
static int s_count;
static volatile uint32_t s_generation;
static volatile bool s_cancel;  // cancels the active download
static char s_running[40];      // id of the download being run, if any
static abs_stream_t *s_stream;

/* ---------- paths and small files ---------- */

static void book_path(const char *id, const char *file, char *out, size_t len)
{
    snprintf(out, len, DL_ROOT "/%s%s%s", id, file ? "/" : "", file ? file : "");
}

static int index_entries(const char *id)
{
    char p[128];
    book_path(id, "index.bin", p, sizeof(p));
    struct stat st;
    return stat(p, &st) == 0 ? st.st_size / 4 : 0;
}

static bool file_exists(const char *id, const char *file)
{
    char p[128];
    book_path(id, file, p, sizeof(p));
    struct stat st;
    return stat(p, &st) == 0;
}

/* ---------- registry (under s_lock) ---------- */

static entry_t *find(const char *id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].id, id) == 0) return &s_entries[i];
    }
    return NULL;
}

static void bump(void)
{
    s_generation++;
}

static void scan(void)
{
    DIR *d = opendir(DL_ROOT);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_count < MAX_DOWNLOADS) {
        if (e->d_type != DT_DIR || strlen(e->d_name) >= sizeof(s_entries[0].id)) continue;
        if (!file_exists(e->d_name, "meta.json") && !file_exists(e->d_name, "queued")) continue;
        entry_t *en = &s_entries[s_count++];
        memset(en, 0, sizeof(*en));
        strlcpy(en->id, e->d_name, sizeof(en->id));
        en->done = index_entries(en->id);
        if (file_exists(en->id, "complete")) {
            en->state = DL_DONE;
            en->total = en->done;
        } else {
            en->state = DL_QUEUED;  // resume
        }
        ESP_LOGI(TAG, "%.8s: %s, %d segments", en->id, en->state == DL_DONE ? "complete" : "resuming", en->done);
    }
    closedir(d);
}

/* ---------- downloading ---------- */

static esp_err_t write_meta(const char *id, const abs_session_t *s, int total)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "duration", s->duration);
    cJSON_AddNumberToObject(root, "segments", total);
    cJSON *chapters = cJSON_AddArrayToObject(root, "chapters");
    for (int i = 0; i < s->chapter_count; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "title", s->chapters[i].title);
        cJSON_AddNumberToObject(c, "start", s->chapters[i].start);
        cJSON_AddNumberToObject(c, "end", s->chapters[i].end);
        cJSON_AddItemToArray(chapters, c);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    char p[128];
    book_path(id, "meta.json", p, sizeof(p));
    esp_err_t err = json ? storage_write_file(p, json, strlen(json)) : ESP_FAIL;
    free(json);
    return err;
}

// (yotwo) Where a download's time goes, to find what limits its speed: waiting for the
// server to answer each request, receiving, and writing to the card. Logged once a minute.
static int64_t s_us_wait, s_us_read, s_us_write, s_stat_since;
static uint32_t s_stat_bytes, s_stat_segments, s_stat_retries;

static int64_t s_us_starved, s_us_blocked;  // writer waiting for data; receiver waiting for room

static void report_speed(void)
{
    const int64_t now = esp_timer_get_time(), span = now - s_stat_since;
    if (span < 60 * 1000000LL) return;
    // The two halves run side by side, so their shares are each of the whole minute.
    ESP_LOGI(TAG, "speed: %u KB/s, %u segments (%.1f x real time), %u retries; receiver: %d%% waiting for the server, "
                  "%d%% receiving, %d%% held up by the writer; writer: %d%% writing to the card, %d%% waiting for data",
             (unsigned)(s_stat_bytes / 1024 * 1000000LL / span), (unsigned)s_stat_segments,
             s_stat_segments * SEGMENT_SECONDS * 1e6 / span, (unsigned)s_stat_retries, (int)(100 * s_us_wait / span),
             (int)(100 * s_us_read / span), (int)(100 * s_us_blocked / span), (int)(100 * s_us_write / span),
             (int)(100 * s_us_starved / span));
    s_us_wait = s_us_read = s_us_write = s_us_starved = s_us_blocked = 0;
    s_stat_bytes = s_stat_segments = s_stat_retries = 0;
    s_stat_since = now;
}

// (yotwo) Receiving and writing are done side by side: a second task fetches whole
// segments into memory while this one writes the ones already fetched to the card. Writing
// is the slow half (the card is wired for SPI), so it should never have to wait for the
// network. A segment reaches the card only once it has arrived whole, which also keeps the
// file free of half-segments from failed attempts.
typedef struct {
    uint8_t *data;  // PSRAM; grown as needed
    size_t cap, len;
    int seg;
} slot_t;

#define SLOTS 3
#define SLOT_START (256 * 1024)
#define SLOT_LIMIT (4 * 1024 * 1024)

typedef struct {
    const char *base;
    int first, total;
    QueueHandle_t free_q, ready_q;  // of slot_t *; a NULL on ready_q means the receiver has finished
    SemaphoreHandle_t done;
    volatile bool stop;    // the writer has given up
    volatile bool failed;  // the receiver could not get a segment
} pipe_t;

// Fetches one segment whole into `slot`. Retries (404/500 while the server transcodes or re-seeks).
static bool fetch_segment(const char *base, int seg, slot_t *slot, const volatile bool *stop)
{
    char path[128];
    snprintf(path, sizeof(path), "%soutput-%d.ts", base, seg);
    for (int attempt = 0; attempt < SEGMENT_RETRIES && !s_cancel && !s_paused && !*stop; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(attempt < 3 ? 500 : 1000));
        if (attempt) s_stat_retries++;
        int64_t t = esp_timer_get_time();
        int status = abs_stream_begin(s_stream, path);
        s_us_wait += esp_timer_get_time() - t + (attempt ? (attempt < 3 ? 500000 : 1000000) : 0);
        if (status != 200) {
            abs_stream_end(s_stream, status > 0);
            continue;
        }
        slot->len = 0;  // drop anything from a failed attempt
        bool ok = true;
        for (;;) {
            if (slot->cap - slot->len < 4096) {
                uint8_t *bigger = slot->cap < SLOT_LIMIT ? heap_caps_realloc(slot->data, slot->cap * 2, MALLOC_CAP_SPIRAM) : NULL;
                if (!bigger) {
                    ESP_LOGE(TAG, "segment %d is too large to hold (%u bytes so far)", seg, (unsigned)slot->len);
                    ok = false;
                    break;
                }
                slot->data = bigger;
                slot->cap *= 2;
            }
            t = esp_timer_get_time();
            int n = abs_stream_read(s_stream, (char *)slot->data + slot->len, 4096);
            s_us_read += esp_timer_get_time() - t;
            if (n < 0 || s_cancel || s_paused || *stop) {
                ok = false;
                break;
            }
            if (n == 0) {
                ok = abs_stream_complete(s_stream);
                break;
            }
            slot->len += n;
        }
        abs_stream_end(s_stream, ok);
        if (ok) return true;
    }
    return false;
}

static void receiver_task(void *arg)
{
    pipe_t *p = arg;
    for (int seg = p->first; seg < p->total && !p->stop && !s_cancel && !s_paused; seg++) {
        slot_t *slot = NULL;
        const int64_t t = esp_timer_get_time();
        while (xQueueReceive(p->free_q, &slot, pdMS_TO_TICKS(200)) != pdTRUE) {
            slot = NULL;
            if (p->stop || s_cancel || s_paused) break;
        }
        s_us_blocked += esp_timer_get_time() - t;
        if (!slot) break;
        slot->seg = seg;
        if (!fetch_segment(p->base, seg, slot, &p->stop)) {
            if (!s_cancel && !s_paused && !p->stop) p->failed = true;
            break;
        }
        xQueueSend(p->ready_q, &slot, portMAX_DELAY);
    }
    slot_t *end = NULL;
    xQueueSend(p->ready_q, &end, portMAX_DELAY);
    xSemaphoreGive(p->done);
    vTaskSuspend(NULL);  // deleted by run()
}

// Makes the audio durable, then records the segments it contains in the index.
static bool commit(FILE *audio, FILE *index, uint32_t *ends, int *pending)
{
    if (*pending == 0) return true;
    const int64_t t = esp_timer_get_time();
    const bool ok = fflush(audio) == 0 && fsync(fileno(audio)) == 0 &&
                    fwrite(ends, 4, *pending, index) == (size_t)*pending && fflush(index) == 0 && fsync(fileno(index)) == 0;
    s_us_write += esp_timer_get_time() - t;  // (yotwo)
    if (!ok) return false;
    *pending = 0;
    report_speed();
    return true;
}

// Downloads (or resumes) one book. Returns true when complete.
static bool run(entry_t *en_snapshot)
{
    char id[40];
    strlcpy(id, en_snapshot->id, sizeof(id));
    abs_session_t session;
    if (abs_start_session(id, NULL, true, &session) != ESP_OK) return false;

    const int total = (int)ceil(session.duration / SEGMENT_SECONDS);
    if (!file_exists(id, "meta.json")) write_meta(id, &session, total);

    char base[96];
    snprintf(base, sizeof(base), "%.*s", (int)(strrchr(session.hls_path, '/') - session.hls_path + 1), session.hls_path);

    char p_audio[128], p_index[128];
    book_path(id, "audio.ts", p_audio, sizeof(p_audio));
    book_path(id, "index.bin", p_index, sizeof(p_index));

    // Resume point: the end of the last committed segment. Anything after it is discarded.
    int done = index_entries(id);
    uint32_t resume = 0;
    FILE *index = fopen(p_index, "a+b");
    if (index && done > 0) {
        fseek(index, (done - 1) * 4, SEEK_SET);
        fread(&resume, 4, 1, index);
        fseek(index, 0, SEEK_END);
    }
    FILE *audio = fopen(p_audio, done ? "r+b" : "wb");
    if (!audio || !index) {
        ESP_LOGE(TAG, "can't open files for %.8s", id);
        if (audio) fclose(audio);
        if (index) fclose(index);
        abs_close_session(session.id, 0, 0);
        abs_free_session(&session);
        return false;
    }
    truncate(p_audio, resume);
    fseek(audio, resume, SEEK_SET);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *en = find(id);
    if (en && en->state == DL_QUEUED) {
        en->state = DL_ACTIVE;
        en->total = total;
        en->done = done;
    }
    bump();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%.8s: downloading from segment %d of %d", id, done, total);
    // (yotwo) Downloads run only on external power, so the radio need not doze between
    // packets as it does to save the battery; dozing delays the answer to every request.
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_us_wait = s_us_read = s_us_write = 0;
    s_stat_bytes = s_stat_segments = s_stat_retries = 0;
    s_stat_since = esp_timer_get_time();

    // Internal RAM: SD writes straight from a PSRAM buffer can be corrupted (see storage.c).
    const int buf_len = 8192;  // (yotwo) was 4096: fewer, larger writes
    uint8_t *buf = heap_caps_malloc(buf_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t ends[SYNC_EVERY];
    int pending = 0;
    bool ok = buf != NULL;

    // (yotwo) The receiver and its segment buffers.
    slot_t slots[SLOTS] = { 0 };
    pipe_t pipe = { .base = base, .first = done, .total = total };
    pipe.free_q = xQueueCreate(SLOTS, sizeof(slot_t *));
    pipe.ready_q = xQueueCreate(SLOTS + 1, sizeof(slot_t *));
    pipe.done = xSemaphoreCreateBinary();
    for (int i = 0; i < SLOTS; i++) {
        slots[i].data = heap_caps_malloc(SLOT_START, MALLOC_CAP_SPIRAM);
        slots[i].cap = slots[i].data ? SLOT_START : 0;
        slot_t *sl = &slots[i];
        if (slots[i].data && pipe.free_q) xQueueSend(pipe.free_q, &sl, 0);
        else ok = false;
    }
    TaskHandle_t receiver = NULL;
    s_us_starved = s_us_blocked = 0;
    // The receiver only talks to the network, so its stack can be in PSRAM; on the other
    // core from this task, which spends most of its time writing to the card.
    if (ok && pipe.ready_q && pipe.done &&
        xTaskCreatePinnedToCoreWithCaps(receiver_task, "dl_recv", 8192, &pipe, 2, &receiver, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        receiver = NULL;
    }
    if (!receiver) ok = false;

    while (receiver) {
        slot_t *slot = NULL;
        int64_t t = esp_timer_get_time();
        xQueueReceive(pipe.ready_q, &slot, portMAX_DELAY);
        s_us_starved += esp_timer_get_time() - t;
        if (!slot) break;  // the receiver has finished
        t = esp_timer_get_time();
        for (size_t off = 0; ok && off < slot->len;) {
            const size_t chunk = slot->len - off < (size_t)buf_len ? slot->len - off : (size_t)buf_len;
            memcpy(buf, slot->data + off, chunk);
            if (fwrite(buf, 1, chunk, audio) != chunk) {
                ESP_LOGE(TAG, "SD write failed (card full?)");
                ok = false;
                s_cancel = true;
            }
            off += chunk;
            // Writing a whole segment is a long stretch on core 0 (the SD driver polls), which
            // starved the idle task there enough for the watchdog to grumble during a flat-out
            // download. A rest every 64 KB lets it run; the cost is far below the card's speed.
            if ((off & 0xFFFF) < (size_t)buf_len) vTaskDelay(1);
        }
        s_us_write += esp_timer_get_time() - t;
        if (ok) {
            s_stat_bytes += slot->len;
            s_stat_segments++;
            ends[pending++] = (uint32_t)ftell(audio);
            if (pending == SYNC_EVERY || slot->seg == total - 1) {
                if (!commit(audio, index, ends, &pending)) {
                    ok = false;
                } else {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    if ((en = find(id))) en->done = slot->seg + 1;
                    bump();
                    xSemaphoreGive(s_lock);
                }
            }
        }
        if (!ok) pipe.stop = true;  // nothing more can be written: the receiver may stop
        xQueueSend(pipe.free_q, &slot, 0);
        // Writing keeps this core busy without a break (the SD driver polls), which starved
        // the idle task until the watchdog complained, four times in one book. A tick's rest
        // after each segment costs under a second a minute.
        vTaskDelay(1);
    }
    if (receiver) {
        xSemaphoreTake(pipe.done, portMAX_DELAY);
        vTaskDeleteWithCaps(receiver);
    }
    if (pipe.failed) ok = false;
    for (int i = 0; i < SLOTS; i++) free(slots[i].data);
    if (pipe.free_q) vQueueDelete(pipe.free_q);
    if (pipe.ready_q) vQueueDelete(pipe.ready_q);
    if (pipe.done) vSemaphoreDelete(pipe.done);
    commit(audio, index, ends, &pending);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // (yotwo) back to saving power
    free(buf);
    fclose(audio);
    fclose(index);
    // An empty close body leaves the user's progress untouched.
    abs_close_session(session.id, 0, 0);
    abs_free_session(&session);

    bool complete = ok && !s_cancel && index_entries(id) >= total;
    if (complete) {
        char p[128];
        book_path(id, "complete", p, sizeof(p));
        storage_write_file(p, "1", 1);
        ESP_LOGI(TAG, "%.8s: complete", id);
    }
    return complete;
}

// Deletes the files of every entry marked for removal (not the one being downloaded).
static void process_removals(void)
{
    for (;;) {
        char id[40] = "";
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_count; i++) {
            if (s_entries[i].state == DL_REMOVING) {
                strlcpy(id, s_entries[i].id, sizeof(id));
                break;
            }
        }
        xSemaphoreGive(s_lock);
        if (!id[0]) return;

        vTaskDelay(pdMS_TO_TICKS(500));  // let the player close the file if it was playing it
        char dir[128];
        book_path(id, NULL, dir, sizeof(dir));
        storage_remove_tree(dir);
        ESP_LOGI(TAG, "%.8s: removed", id);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        entry_t *en = find(id);
        if (en) *en = s_entries[--s_count];  // swap-remove
        bump();
        xSemaphoreGive(s_lock);
    }
}

static void download_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        for (;;) {
            process_removals();
            if (!wifi_is_connected() || s_paused) break;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            entry_t *next = NULL;
            for (int i = 0; i < s_count && !next; i++) {
                if (s_entries[i].state == DL_QUEUED) next = &s_entries[i];
            }
            entry_t snapshot = next ? *next : (entry_t){0};
            strlcpy(s_running, snapshot.id, sizeof(s_running));
            s_cancel = false;
            xSemaphoreGive(s_lock);
            if (!next) break;

            bool complete = run(&snapshot);

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_running[0] = 0;
            entry_t *en = find(snapshot.id);
            bool failed = false;
            if (en && en->state != DL_REMOVING) {
                if (complete) {
                    en->state = DL_DONE;
                    en->done = en->total;
                } else if (s_paused) {
                    en->state = DL_QUEUED;  // (yotwo) held, not failed: carries on from here later
                } else if (++en->failures >= MAX_FAILURES) {
                    en->state = DL_ERROR;
                } else {
                    en->state = DL_QUEUED;
                    failed = true;
                }
            }
            bump();
            xSemaphoreGive(s_lock);
            if (failed) break;  // back off before retrying
        }
    }
}

/* ---------- public ---------- */

void download_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!storage_ready()) return;
    storage_mkdirs(DL_ROOT);
    scan();
    s_stream = abs_stream_create();
    // (yotwo) Stack in internal memory, not PSRAM as in ABSpresso: this task is on the SD card
    // all the time, and on this chip SD calls from a PSRAM stack have crashed twice in other
    // tasks (see hardware/storage.h).
    xTaskCreatePinnedToCoreWithCaps(download_task, "download", 8192, NULL, 2, &s_task, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// (yotwo)
int download_list(dl_info_t *out, int max)
{
    if (!s_lock) return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && n < max; i++) {
        strlcpy(out[n].id, s_entries[i].id, sizeof(out[n].id));
        out[n].state = s_entries[i].state;
        out[n].percent = s_entries[i].state == DL_DONE ? 100
                         : s_entries[i].total ? 100 * s_entries[i].done / s_entries[i].total : 0;
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

// (yotwo)
void download_set_paused(bool paused)
{
    s_paused = paused;
    if (!paused && s_task) xTaskNotifyGive(s_task);
}

esp_err_t download_start(const abs_book_t *book)
{
    if (!storage_ready() || !s_task) return ESP_ERR_INVALID_STATE;
    char dir[128], marker[128];
    book_path(book->id, NULL, dir, sizeof(dir));
    book_path(book->id, "queued", marker, sizeof(marker));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *en = find(book->id);
    if (!en && s_count < MAX_DOWNLOADS) {
        en = &s_entries[s_count++];
        memset(en, 0, sizeof(*en));
        strlcpy(en->id, book->id, sizeof(en->id));
    }
    if (en && (en->state == DL_NONE || en->state == DL_ERROR)) {
        en->state = DL_QUEUED;
        en->failures = 0;
    }
    bump();
    xSemaphoreGive(s_lock);
    if (!en) return ESP_ERR_NO_MEM;

    // A marker so a queued-but-not-started download survives a reboot.
    storage_mkdirs(dir);
    storage_write_file(marker, "1", 1);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void download_remove(const char *item_id)
{
    if (!storage_ready() || !s_task) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *en = find(item_id);
    if (en) {
        en->state = DL_REMOVING;
        if (strcmp(s_running, item_id) == 0) s_cancel = true;
    }
    bump();
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

dl_state_t download_state(const char *item_id, int *percent)
{
    dl_state_t st = DL_NONE;
    int pct = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        entry_t *en = find(item_id);
        if (en) {
            st = en->state;
            pct = en->total ? (100 * en->done / en->total) : 0;
        }
        xSemaphoreGive(s_lock);
    }
    if (percent) *percent = pct;
    return st;
}

bool download_busy(void)
{
    if (!s_lock) return false;
    bool busy = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && !busy; i++) {
        busy = s_entries[i].state == DL_QUEUED || s_entries[i].state == DL_ACTIVE || s_entries[i].state == DL_REMOVING;
    }
    xSemaphoreGive(s_lock);
    return busy;
}

uint32_t download_generation(void)
{
    return s_generation;
}

esp_err_t download_load_meta(const char *item_id, abs_session_t *out)
{
    memset(out, 0, sizeof(*out));
    char p[128];
    book_path(item_id, "meta.json", p, sizeof(p));
    char *json = storage_read_file(p, NULL);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!root) return ESP_FAIL;
    out->duration = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "duration"));
    const cJSON *chapters = cJSON_GetObjectItem(root, "chapters");
    int n = cJSON_GetArraySize(chapters);
    if (n > 0) {
        out->chapters = heap_caps_calloc(n, sizeof(abs_chapter_t), MALLOC_CAP_SPIRAM);
        const cJSON *c;
        cJSON_ArrayForEach(c, chapters) {
            if (!out->chapters) break;
            abs_chapter_t *ch = &out->chapters[out->chapter_count++];
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(c, "title"));
            ch->title = strdup(t ? t : "");
            ch->start = cJSON_GetNumberValue(cJSON_GetObjectItem(c, "start"));
            ch->end = cJSON_GetNumberValue(cJSON_GetObjectItem(c, "end"));
        }
    }
    cJSON_Delete(root);
    return out->duration > 0 ? ESP_OK : ESP_FAIL;
}

void download_audio_path(const char *item_id, char *path, size_t len)
{
    book_path(item_id, "audio.ts", path, len);
}

esp_err_t download_segment_offset(const char *item_id, int seg, uint32_t *offset)
{
    *offset = 0;
    if (seg <= 0) return ESP_OK;
    char p[128];
    book_path(item_id, "index.bin", p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_FAIL;
    bool ok = fseek(f, (seg - 1) * 4, SEEK_SET) == 0 && fread(offset, 4, 1, f) == 1;
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}

bool download_get_progress(const char *item_id, double *position, bool *pending)
{
    char p[128];
    book_path(item_id, "progress.json", p, sizeof(p));
    char *json = storage_read_file(p, NULL);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    free(json);
    if (!root) return false;
    *position = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "position"));
    *pending = cJSON_IsTrue(cJSON_GetObjectItem(root, "pending"));
    cJSON_Delete(root);
    return true;
}

void download_set_progress(const char *item_id, double position, bool pending)
{
    char p[128], json[64];
    book_path(item_id, "progress.json", p, sizeof(p));
    snprintf(json, sizeof(json), "{\"position\":%.2f,\"pending\":%s}", position, pending ? "true" : "false");
    storage_write_file(p, json, strlen(json));
}

void download_sync_progress(abs_book_t *books, int count, bool online)
{
    if (!storage_ready()) return;
    for (int i = 0; i < count; i++) {
        double pos;
        bool pending;
        if (download_state(books[i].id, NULL) != DL_DONE || !download_get_progress(books[i].id, &pos, &pending) ||
            !pending) {
            continue;
        }
        // Listening that happened offline wins over what the server last heard.
        books[i].current_time = pos;
        books[i].progress = books[i].duration > 0 ? pos / books[i].duration : 0;
        if (online && abs_patch_progress(books[i].id, pos, books[i].duration, false) == ESP_OK) {
            download_set_progress(books[i].id, pos, false);
            ESP_LOGI(TAG, "%.8s: pushed offline position %.0f s", books[i].id, pos);
        }
    }
}
