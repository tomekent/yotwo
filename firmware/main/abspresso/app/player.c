// Audiobook playback over Audiobookshelf's HLS transcode.
//
// ABS serves fixed 6 s MPEG-TS segments named output-N.ts, and segment N always starts at N*6 s
// even after the server restarts its transcoder for a seek. So the playlist is never fetched:
// playing from time t means fetching segments from floor(t/6) and dropping (t mod 6) s of audio.
//
//   fetch task:  HTTPS segment GETs -> stream buffer (PSRAM)
//   decode task: stream buffer -> TS demux + AAC/MP3 decode -> I2S
//   control task: commands, session lifecycle, progress sync, status

#include "player.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "board.h"
#include "download.h"
#include "wifi.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "player";

#define SEGMENT_SECONDS     6.0
#define STREAM_BUF_SIZE     (512 * 1024)   // about a minute at typical audiobook bitrates
#define SYNC_INTERVAL_US    (20 * 1000000LL)
#define SEGMENT_MAX_RETRIES 40
#define START_THRESHOLD     (48 * 1024)    // bytes buffered before the first decode

// Byte-addressable internal RAM. MALLOC_CAP_INTERNAL alone isn't enough on the original ESP32,
// where it can return the IRAM heap, which only allows aligned 32-bit access (a memcpy into it
// raises LoadStoreError).
#define INTERNAL_RAM (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

typedef enum {
    CMD_OPEN,
    CMD_TOGGLE,
    CMD_SEEK_REL,
    CMD_SEEK_ABS,
    CMD_CHAPTER,
    CMD_STOP,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    double arg;
    char id[40];
    char title[128];
    char author[96];
    char episode_id[40];
    double book_time;  // the server's saved position for the book
} cmd_t;

#define BIT_FETCH_IDLE  BIT0
#define BIT_DECODE_IDLE BIT1

static QueueHandle_t s_cmds;
static SemaphoreHandle_t s_lock;
static player_status_t s_status;

// Pipeline. Parameters are only written while both workers are idle.
static StreamBufferHandle_t s_sbuf;
static EventGroupHandle_t s_idle;
static TaskHandle_t s_fetch_task, s_decode_task;
static volatile bool s_abort;
static volatile bool s_paused;
static char s_seg_base[96];
static int s_start_seg, s_last_seg;
static double s_skip_seconds;

// Pipeline outputs.
static volatile bool s_fetch_done, s_fetch_failed, s_decode_done, s_underrun, s_audio_started;
static volatile int64_t s_frames;   // frames decoded since the start segment
static volatile int s_rate;

#ifdef PLAYER_STATUS_LOG
static volatile uint32_t s_dbg_bytes_in, s_dbg_dec_err, s_dbg_write_max_us, s_dbg_underruns;
static double s_dbg_env_sum;
static int s_dbg_env_frames, s_dbg_env_block;
#endif

// Control-task state.
static abs_session_t s_session;
static bool s_have_session;
static bool s_local;          // playing a download from the SD card (no server session)
static double s_book_time;    // server position of the open book, for reopening
static abs_stream_t *s_net;
static int64_t s_volume_changed_us;  // when the volume last changed and isn't saved yet (0 = saved)
#define VOLUME_SAVE_DELAY_US (2 * 1000000LL)
static void save_volume_if_settled(int64_t now);
static double s_seg_start_time;
static double s_listen_since_sync;
static int64_t s_last_sync_us, s_last_tick_us;

/* ---------- fetch ---------- */

static bool send_all(const uint8_t *p, int len)
{
    while (len > 0 && !s_abort) {
        size_t n = xStreamBufferSend(s_sbuf, p, len, pdMS_TO_TICKS(100));
        p += n;
        len -= n;
    }
    return len == 0;
}

static void sleep_unless_abort(int ms)
{
    for (int t = 0; t < ms && !s_abort; t += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Streams one segment into the buffer. Retries on errors, skipping bytes already delivered.
static bool fetch_segment(int seg, char *buf, int buf_len)
{
    char path[128];
    snprintf(path, sizeof(path), "%soutput-%d.ts", s_seg_base, seg);
    int delivered = 0;
    for (int attempt = 0; attempt < SEGMENT_MAX_RETRIES && !s_abort; attempt++) {
        if (attempt) {
            sleep_unless_abort(attempt < 3 ? 500 : 1000);
        }
        int status = abs_stream_begin(s_net, path);
        if (status != 200) {
            // 404: not transcoded yet. 500: the server restarted its transcoder for our seek.
            ESP_LOGD(TAG, "%s -> %d", path, status);
            abs_stream_end(s_net, status > 0);
            continue;
        }
        int offset = 0;
        bool ok = true;
        while (!s_abort) {
            int n = abs_stream_read(s_net, buf, buf_len);
            if (n < 0) {
                ok = false;
                break;
            }
            if (n == 0) {
                ok = abs_stream_complete(s_net);
                break;
            }
            int skip = delivered - offset;
            offset += n;
            if (skip >= n) continue;
            if (skip < 0) skip = 0;
            if (!send_all((uint8_t *)buf + skip, n - skip)) break;
            delivered += n - skip;
        }
        if (s_abort) {
            abs_stream_end(s_net, false);
            return false;
        }
        abs_stream_end(s_net, ok);
        if (ok) {
            return true;
        }
        ESP_LOGW(TAG, "segment %d interrupted after %d bytes, retrying", seg, delivered);
    }
    return false;
}

// Downloaded book: the same TS stream, read from the SD card from the start segment onwards.
static void fetch_local(char *buf, int buf_len)
{
    char path[128];
    uint32_t offset;
    download_audio_path(s_status.item_id, path, sizeof(path));
    FILE *f = download_segment_offset(s_status.item_id, s_start_seg, &offset) == ESP_OK ? fopen(path, "rb") : NULL;
    if (!f || fseek(f, offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "can't read downloaded audio %s", path);
        if (f) fclose(f);
        s_fetch_failed = true;
        return;
    }
    size_t n;
    while (!s_abort && (n = fread(buf, 1, buf_len, f)) > 0) {
        if (!send_all((uint8_t *)buf, n)) break;
    }
    fclose(f);
}

static void fetch_task(void *arg)
{
    const int buf_len = 4096;
    char *buf = heap_caps_malloc(buf_len, INTERNAL_RAM);
    for (;;) {
        xEventGroupSetBits(s_idle, BIT_FETCH_IDLE);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_local) {
            fetch_local(buf, buf_len);
            if (!s_abort && !s_fetch_failed) s_fetch_done = true;
            continue;
        }
        for (int seg = s_start_seg; seg <= s_last_seg && !s_abort; seg++) {
            if (!fetch_segment(seg, buf, buf_len)) {
                if (!s_abort) {
                    ESP_LOGE(TAG, "giving up on segment %d", seg);
                    s_fetch_failed = true;
                }
                break;
            }
        }
        if (!s_abort && !s_fetch_failed) {
            s_fetch_done = true;
        }
    }
}

/* ---------- decode ---------- */

static void decode_run(uint8_t *in, int in_len, uint8_t **pcm, uint32_t *pcm_len)
{
    esp_audio_simple_dec_cfg_t cfg = {.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_TS};
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed");
        s_fetch_failed = true;
        return;
    }
    int channels = 0;
    int64_t skip_frames = 0;
    bool primed = false;

    while (!s_abort) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Build a little cushion before starting (and after running dry) so playback doesn't stutter.
        if (!primed) {
            if (xStreamBufferBytesAvailable(s_sbuf) < START_THRESHOLD && !s_fetch_done) {
                s_underrun = true;
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            primed = true;
        }
        size_t n = xStreamBufferReceive(s_sbuf, in, in_len, pdMS_TO_TICKS(50));
        if (n == 0) {
            if (s_fetch_done) {
                s_decode_done = true;
                break;
            }
            s_underrun = true;
            primed = false;
#ifdef PLAYER_STATUS_LOG
            s_dbg_underruns++;
#endif
            continue;
        }
        s_underrun = false;
#ifdef PLAYER_STATUS_LOG
        s_dbg_bytes_in += n;
#endif

        // Keep calling until the whole chunk is consumed, as the library's own tests do: a call can
        // legitimately consume and output nothing while the parser advances, and `in` must not be
        // overwritten while any of it is unconsumed.
        esp_audio_simple_dec_raw_t raw = {.buffer = in, .len = n};
        int idle_calls = 0;
        while (raw.len > 0 && !s_abort) {
            esp_audio_simple_dec_out_t out = {.buffer = *pcm, .len = *pcm_len};
            esp_audio_err_t ret = esp_audio_simple_dec_process(dec, &raw, &out);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *bigger = heap_caps_realloc(*pcm, out.needed_size, INTERNAL_RAM);
                if (!bigger) break;
                *pcm = bigger;
                *pcm_len = out.needed_size;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
#ifdef PLAYER_STATUS_LOG
                s_dbg_dec_err++;
#endif
                ESP_LOGW(TAG, "decode error %d", ret);
                // Skip past the bad data (at least one 188-byte TS packet) rather than the whole buffer.
                uint32_t skip = raw.consumed ? raw.consumed : (raw.len < 188 ? raw.len : 188);
                raw.buffer += skip;
                raw.len -= skip;
                continue;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (out.decoded_size == 0) {
                if (raw.consumed == 0 && ++idle_calls > 64) {
                    ESP_LOGW(TAG, "decoder made no progress on %lu bytes, dropping them", raw.len);
                    break;
                }
                continue;
            }
            idle_calls = 0;
            if (!channels) {
                esp_audio_simple_dec_info_t info = {0};
                esp_audio_simple_dec_get_info(dec, &info);
                ESP_LOGI(TAG, "stream: %lu Hz, %d ch, %d bit", info.sample_rate, info.channel, info.bits_per_sample);
                if (info.bits_per_sample != 16 || board_audio_open(info.sample_rate, info.channel, 16) != ESP_OK) {
                    s_fetch_failed = true;
                    goto out;
                }
                channels = info.channel;
                s_rate = info.sample_rate;
                skip_frames = (int64_t)(s_skip_seconds * info.sample_rate);
            }
            const int frame_bytes = 2 * channels;
            int frames = out.decoded_size / frame_bytes;
            int drop = skip_frames < frames ? (int)skip_frames : frames;
            skip_frames -= drop;
#ifdef PLAYER_STATUS_LOG
            // Loudness envelope in 0.5 s blocks, to compare against a host decode of the same segments.
            for (int i = 0; i < frames * channels; i++) {
                double v = ((int16_t *)out.buffer)[i];
                s_dbg_env_sum += v * v;
            }
            s_dbg_env_frames += frames;
            if (s_dbg_env_frames >= s_rate / 2 && s_dbg_env_block < 60) {
                printf("ENV %d %.0f\n", s_dbg_env_block++, sqrt(s_dbg_env_sum / (s_dbg_env_frames * channels)));
                s_dbg_env_sum = 0;
                s_dbg_env_frames = 0;
            }
            int64_t w0 = esp_timer_get_time();
#endif
            if (frames > drop) {
                board_audio_write((int16_t *)(out.buffer + drop * frame_bytes), (frames - drop) * frame_bytes);
                s_audio_started = true;
            }
            s_frames += frames;
#ifdef PLAYER_STATUS_LOG
            uint32_t wus = esp_timer_get_time() - w0;
            if (wus > s_dbg_write_max_us) s_dbg_write_max_us = wus;
#endif
        }
    }
out:
    esp_audio_simple_dec_close(dec);
}

static void decode_task(void *arg)
{
    const int in_len = 4096;
    uint8_t *in = heap_caps_malloc(in_len, INTERNAL_RAM);
    uint32_t pcm_len = 8192;
    uint8_t *pcm = heap_caps_malloc(pcm_len, INTERNAL_RAM);
    for (;;) {
        xEventGroupSetBits(s_idle, BIT_DECODE_IDLE);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        decode_run(in, in_len, &pcm, &pcm_len);
    }
}

/* ---------- pipeline control (control task only) ---------- */

static void pipeline_stop(void)
{
    s_abort = true;
    xEventGroupWaitBits(s_idle, BIT_FETCH_IDLE | BIT_DECODE_IDLE, pdFALSE, pdTRUE, portMAX_DELAY);
    xStreamBufferReset(s_sbuf);
}

static double current_position(void)
{
    int rate = s_rate;
    return rate ? s_seg_start_time + (double)s_frames / rate : s_seg_start_time + s_skip_seconds;
}

static void pipeline_start(double t)
{
    pipeline_stop();
    if (t < 0) t = 0;
    if (t > s_session.duration - 1) t = fmax(0, s_session.duration - 1);
    snprintf(s_seg_base, sizeof(s_seg_base), "%.*s", (int)(strrchr(s_session.hls_path, '/') - s_session.hls_path + 1),
             s_session.hls_path);
    s_start_seg = (int)(t / SEGMENT_SECONDS);
    s_last_seg = (int)ceil(s_session.duration / SEGMENT_SECONDS) - 1;
    s_seg_start_time = s_start_seg * SEGMENT_SECONDS;
    s_skip_seconds = t - s_seg_start_time;
    s_frames = 0;
    s_rate = 0;
    s_fetch_done = s_fetch_failed = s_decode_done = s_audio_started = false;
    s_underrun = true;
    ESP_LOGI(TAG, "play from %.1f s (segment %d of %d)", t, s_start_seg, s_last_seg);

    xEventGroupClearBits(s_idle, BIT_FETCH_IDLE | BIT_DECODE_IDLE);
    s_abort = false;
    xTaskNotifyGive(s_fetch_task);
    xTaskNotifyGive(s_decode_task);
}

/* ---------- status helpers ---------- */

static void set_state(player_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = st;
    xSemaphoreGive(s_lock);
}

static void update_status(void)
{
    if (!s_have_session) return;  // keep showing the last position
    double pos = current_position();
    int ci = -1;
    for (int i = 0; i < s_session.chapter_count; i++) {
        if (pos >= s_session.chapters[i].start && pos < s_session.chapters[i].end) {
            ci = i;
            break;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.position = pos;
    s_status.duration = s_session.duration;
    s_status.chapter_index = ci;
    s_status.chapter_count = s_session.chapter_count;
    if (ci >= 0) {
        strlcpy(s_status.chapter, s_session.chapters[ci].title, sizeof(s_status.chapter));
        s_status.chapter_start = s_session.chapters[ci].start;
        s_status.chapter_end = s_session.chapters[ci].end;
    } else {
        s_status.chapter[0] = 0;
        s_status.chapter_start = 0;
        s_status.chapter_end = s_session.duration;
    }
    s_status.buffer_percent = s_sbuf ? (int)(100 * xStreamBufferBytesAvailable(s_sbuf) / STREAM_BUF_SIZE) : 0;

    player_state_t st = s_status.state;
    if (st == PLAYER_PLAYING || st == PLAYER_BUFFERING) {
        if (s_fetch_failed) {
            st = PLAYER_ERROR;
        } else if (s_decode_done) {
            st = PLAYER_FINISHED;
        } else {
            st = (s_underrun || !s_audio_started) ? PLAYER_BUFFERING : PLAYER_PLAYING;
        }
        s_status.state = st;
    }
    xSemaphoreGive(s_lock);
}

/* ---------- server updates (sync worker) ---------- */

// Progress syncs and session closes are network calls that can take a while; the control task
// must stay free to react to taps, so it queues them here and a worker sends them in order.
typedef enum { JOB_SYNC, JOB_CLOSE, JOB_PATCH } job_type_t;

typedef struct {
    job_type_t type;
    char session[40];
    char item[40];
    double position, listened, duration;
    bool finished;
} sync_job_t;

static QueueHandle_t s_jobs;

static void queue_job(const sync_job_t *job)
{
    if (xQueueSend(s_jobs, job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "sync queue full, dropping an update");
    }
}

static void sync_worker(void *arg)
{
    sync_job_t j;
    for (;;) {
        xQueueReceive(s_jobs, &j, portMAX_DELAY);
        switch (j.type) {
        case JOB_SYNC:
            abs_sync_session(j.session, j.position, j.listened, j.duration);
            break;
        case JOB_CLOSE:
            abs_close_session(j.session, j.position, j.listened);
            break;
        case JOB_PATCH:
            // Downloaded books: the local copy is already saved as pending; clear that once sent.
            if (wifi_is_connected() && abs_patch_progress(j.item, j.position, j.duration, j.finished) == ESP_OK) {
                download_set_progress(j.item, j.position, false);
            }
            break;
        }
    }
}

static void sync_progress(bool force)
{
#ifdef PLAYER_NO_SYNC
    s_listen_since_sync = 0;
    return;
#endif
    if (!s_have_session || s_listen_since_sync <= 0) return;
    int64_t now = esp_timer_get_time();
    if (!force && now - s_last_sync_us < SYNC_INTERVAL_US) return;
    s_last_sync_us = now;
    sync_job_t job = {.position = current_position(), .listened = s_listen_since_sync, .duration = s_session.duration};
    if (s_local) {
        // Downloads: save locally first (works offline), then tell the server if we can.
        download_set_progress(s_status.item_id, job.position, true);
        job.type = JOB_PATCH;
        strlcpy(job.item, s_status.item_id, sizeof(job.item));
    } else {
        job.type = JOB_SYNC;
        strlcpy(job.session, s_session.id, sizeof(job.session));
    }
    queue_job(&job);
    s_listen_since_sync = 0;
}

static void close_session(void)
{
    if (!s_have_session) return;
    pipeline_stop();
    if (s_local) {
        sync_progress(true);
    } else {
        sync_job_t job = {.type = JOB_CLOSE, .position = current_position(), .listened = s_listen_since_sync};
        strlcpy(job.session, s_session.id, sizeof(job.session));
        queue_job(&job);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);  // player_get_chapter() reads the chapters
    abs_free_session(&s_session);
    s_have_session = false;
    xSemaphoreGive(s_lock);
    s_listen_since_sync = 0;
    board_audio_close();
}

static bool open_session(const char *item_id)
{
    set_state(PLAYER_LOADING);
    const char *episode = s_status.episode_id;  // only the control task writes it
    // Downloads are books only; episodes always stream.
    s_local = !episode[0] && download_state(item_id, NULL) == DL_DONE &&
              download_load_meta(item_id, &s_session) == ESP_OK;
    if (s_local) {
        // Start from any offline listening not yet on the server, else the server's position.
        double pos;
        bool pending;
        s_session.current_time = (download_get_progress(item_id, &pos, &pending) && pending) ? pos : s_book_time;
        // (yotwo) With nothing waiting to be sent, the server knows best where the listener
        // is: the book may have been heard elsewhere since the library was loaded, or opened
        // by a card without its library, when s_book_time is 0. Starting there would play
        // from the wrong place and then tell the server so.
        double server_time;
        if (!(download_get_progress(item_id, &pos, &pending) && pending) && wifi_is_connected() &&
            abs_get_progress(item_id, &server_time) == ESP_OK) {
            s_session.current_time = server_time;
        }
        ESP_LOGI(TAG, "playing download from %.0f s", s_session.current_time);
        s_have_session = true;
        s_last_sync_us = esp_timer_get_time();
        return true;
    }
    if (abs_start_session(item_id, episode, false, &s_session) != ESP_OK) {
        set_state(PLAYER_ERROR);
        return false;
    }
    if (episode[0] && s_session.display_title[0]) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_status.title, s_session.display_title, sizeof(s_status.title));
        if (s_session.display_author[0]) strlcpy(s_status.author, s_session.display_author, sizeof(s_status.author));
        xSemaphoreGive(s_lock);
    }
    s_have_session = true;
    s_last_sync_us = esp_timer_get_time();
    return true;
}

static void seek_to(double t)
{
    if (!s_have_session) return;
    sync_progress(true);
    pipeline_start(t);
    set_state(s_paused ? PLAYER_PAUSED : PLAYER_BUFFERING);
}

/* ---------- control task ---------- */

static void handle(const cmd_t *c)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    player_state_t st = s_status.state;
    xSemaphoreGive(s_lock);

    switch (c->type) {
    case CMD_OPEN:
        if (s_have_session && strcmp(c->id, s_status.item_id) == 0 && strcmp(c->episode_id, s_status.episode_id) == 0 &&
            st != PLAYER_ERROR) {
            break;  // already loaded
        }
        close_session();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_status.item_id, c->id, sizeof(s_status.item_id));
        strlcpy(s_status.episode_id, c->episode_id, sizeof(s_status.episode_id));
        strlcpy(s_status.title, c->title, sizeof(s_status.title));
        strlcpy(s_status.author, c->author, sizeof(s_status.author));
        s_status.chapter[0] = 0;
        s_status.position = 0;
        xSemaphoreGive(s_lock);
        s_book_time = c->book_time;
        if (open_session(c->id)) {
            s_paused = false;
            pipeline_start(s_session.current_time);
            set_state(PLAYER_BUFFERING);
        }
        break;

    case CMD_TOGGLE:
        if (st == PLAYER_ERROR && s_status.item_id[0]) {
            // Reopen at our last known position; the old session may be gone server-side.
            double pos = s_have_session ? current_position() : -1;
            close_session();
            if (open_session(s_status.item_id)) {
                s_paused = false;
                pipeline_start(pos >= 0 ? pos : s_session.current_time);
                set_state(PLAYER_BUFFERING);
            }
        } else if (st == PLAYER_PAUSED) {
            s_paused = false;
            set_state(PLAYER_BUFFERING);
        } else if (st == PLAYER_PLAYING || st == PLAYER_BUFFERING) {
            s_paused = true;
            set_state(PLAYER_PAUSED);
            sync_progress(true);
        } else if (st == PLAYER_FINISHED && s_status.item_id[0]) {
            close_session();
            if (open_session(s_status.item_id)) {
                s_paused = false;
                pipeline_start(0);
                set_state(PLAYER_BUFFERING);
            }
        }
        break;

    case CMD_SEEK_REL:
        seek_to(current_position() + c->arg);
        break;

    case CMD_SEEK_ABS:
        seek_to(c->arg);
        break;

    case CMD_CHAPTER: {
        if (!s_have_session || s_session.chapter_count == 0) break;
        double pos = current_position();
        int ci = 0;
        for (int i = 0; i < s_session.chapter_count; i++) {
            if (pos >= s_session.chapters[i].start) ci = i;
        }
        if (c->arg < 0 && pos - s_session.chapters[ci].start > 3) {
            // First press restarts the current chapter, like most players.
        } else {
            ci += (int)c->arg;
        }
        if (ci < 0) ci = 0;
        if (ci >= s_session.chapter_count) break;
        seek_to(s_session.chapters[ci].start);
        break;
    }

    case CMD_STOP:
        close_session();
        s_paused = false;
        set_state(PLAYER_IDLE);
        break;

    }
}

static void control_task(void *arg)
{
    s_last_tick_us = esp_timer_get_time();
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmds, &c, pdMS_TO_TICKS(250))) {
            handle(&c);
        }
        update_status();

        int64_t now = esp_timer_get_time();
        save_volume_if_settled(now);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        player_state_t st = s_status.state;
        xSemaphoreGive(s_lock);
        if (st == PLAYER_PLAYING) {
            s_listen_since_sync += (now - s_last_tick_us) / 1e6;
        }
        s_last_tick_us = now;

#ifdef PLAYER_STATUS_LOG  // define to log pipeline stats every second
        static int64_t last_log;
        if (now - last_log > 1000000 && s_have_session) {
            last_log = now;
            ESP_LOGI(TAG, "st %d pos %.1f buf %d%% in %lu B/s err %lu wmax %lu us undr %lu int %u", st,
                     current_position(), (int)(100 * xStreamBufferBytesAvailable(s_sbuf) / STREAM_BUF_SIZE),
                     s_dbg_bytes_in, s_dbg_dec_err, s_dbg_write_max_us, s_dbg_underruns,
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_dbg_bytes_in = s_dbg_write_max_us = 0;
        }
#endif
        if (st == PLAYER_PLAYING) {
            sync_progress(false);
        } else if (st == PLAYER_FINISHED && s_have_session) {
            ESP_LOGI(TAG, "book finished");
            const bool local = s_local;
            const double duration = s_session.duration;
            close_session();
            if (local) {
                sync_job_t job = {.type = JOB_PATCH, .position = duration, .duration = duration, .finished = true};
                strlcpy(job.item, s_status.item_id, sizeof(job.item));
                queue_job(&job);
            }
        }
    }
}

/* ---------- public ---------- */

void player_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    s_idle = xEventGroupCreate();
    s_net = abs_stream_create();

    uint8_t *storage = heap_caps_malloc(STREAM_BUF_SIZE + 1, MALLOC_CAP_SPIRAM);
    static StaticStreamBuffer_t sb_struct;
    s_sbuf = xStreamBufferCreateStatic(STREAM_BUF_SIZE, 1, storage, &sb_struct);

    // The TS demuxer opens its inner AAC/MP3 decoder through the base registry, so register both.
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    uint8_t vol = 60;
    nvs_handle_t h;
    if (nvs_open("player", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "volume", &vol);
        nvs_close(h);
    }
    s_status.volume = vol;
    s_status.chapter_index = -1;
    board_audio_set_volume(vol);

    // Network on core 0 with Wi-Fi; decoding on core 1 above LVGL so audio never starves.
    xTaskCreatePinnedToCore(fetch_task, "abs_fetch", 8192, NULL, 5, &s_fetch_task, 0);
    xTaskCreatePinnedToCore(decode_task, "abs_decode", 16384, NULL, 6, &s_decode_task, 1);
    s_jobs = xQueueCreate(8, sizeof(sync_job_t));
    xTaskCreatePinnedToCore(control_task, "abs_ctrl", 8192, NULL, 4, NULL, 0);
    // Stack in PSRAM: network and SD work only, never the internal flash.
    xTaskCreatePinnedToCoreWithCaps(sync_worker, "abs_sync", 8192, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);
}

static void post(cmd_type_t type, double arg)
{
    cmd_t c = {.type = type, .arg = arg};
    xQueueSend(s_cmds, &c, 0);
}

void player_open(const abs_book_t *book)
{
    cmd_t c = {.type = CMD_OPEN};
    strlcpy(c.id, book->id, sizeof(c.id));
    strlcpy(c.title, book->title, sizeof(c.title));
    strlcpy(c.author, book->author, sizeof(c.author));
    c.book_time = book->current_time;
    if (book->podcast) {
        // Shows play an episode: carry on with the latest one (the UI picks others explicitly).
        strlcpy(c.episode_id, book->resume_episode, sizeof(c.episode_id));
        if (!c.episode_id[0]) return;
    }
    xQueueSend(s_cmds, &c, 0);
}

void player_open_episode(const char *item_id, const char *episode_id, const char *title, const char *show)
{
    cmd_t c = {.type = CMD_OPEN};
    strlcpy(c.id, item_id, sizeof(c.id));
    strlcpy(c.episode_id, episode_id, sizeof(c.episode_id));
    strlcpy(c.title, title, sizeof(c.title));
    strlcpy(c.author, show, sizeof(c.author));
    xQueueSend(s_cmds, &c, 0);
}

void player_toggle(void) { post(CMD_TOGGLE, 0); }
void player_seek_relative(double seconds) { post(CMD_SEEK_REL, seconds); }
void player_seek_to(double seconds) { post(CMD_SEEK_ABS, seconds); }
void player_chapter_step(int delta) { post(CMD_CHAPTER, delta); }
void player_stop(void) { post(CMD_STOP, 0); }

void player_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    // Applied at once (dragging the volume arc sends many of these); saved later by the control
    // task once it stops changing, to spare the flash.
    board_audio_set_volume(volume);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.volume = volume;
    s_volume_changed_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

static void save_volume_if_settled(int64_t now)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool due = s_volume_changed_us && now - s_volume_changed_us > VOLUME_SAVE_DELAY_US;
    const int v = s_status.volume;
    if (due) s_volume_changed_us = 0;
    xSemaphoreGive(s_lock);
    if (!due) return;
    nvs_handle_t h;
    if (nvs_open("player", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "volume", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool player_get_chapter(int index, char *title, size_t len, double *start, double *end)
{
    if (!s_lock) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_have_session && index >= 0 && index < s_session.chapter_count) {
        const abs_chapter_t *c = &s_session.chapters[index];
        if (title && len) strlcpy(title, c->title ? c->title : "", len);
        if (start) *start = c->start;
        if (end) *end = c->end;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void player_get_status(player_status_t *out)
{
    if (!s_lock) {  // not initialised yet (the UI starts first)
        memset(out, 0, sizeof(*out));
        out->chapter_index = -1;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
