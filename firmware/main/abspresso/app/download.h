#pragma once

// Downloads books to the SD card for offline playback.
//
// A download is the book's HLS transcode (the same MPEG-TS segments the player streams), saved as
// STORAGE_ROOT/dl/<id>/audio.ts plus index.bin (end offset of each completed segment) and
// meta.json (title, duration, chapters). Downloads run one at a time in a background task and
// resume after a reboot. Listening position for downloaded books is kept in progress.json and
// pushed to the server whenever it's reachable.

#include <stdbool.h>
#include <stdint.h>
#include "abs_api.h"
#include "esp_err.h"

typedef enum {
    DL_NONE,
    DL_QUEUED,
    DL_ACTIVE,
    DL_DONE,
    DL_ERROR,
    DL_REMOVING,  // being cancelled/deleted in the background
} dl_state_t;

void download_init(void);

// Queues a download (no-op if already queued or done). Fails without an SD card.
esp_err_t download_start(const abs_book_t *book);
// Cancels an in-progress download and/or deletes the downloaded files. Returns immediately;
// the deletion happens in the background.
void download_remove(const char *item_id);
dl_state_t download_state(const char *item_id, int *percent);
// (yotwo) Every download known (queued, running, complete or failed), up to `max`.
typedef struct {
    char id[40];
    dl_state_t state;
    int percent;
} dl_info_t;
int download_list(dl_info_t *out, int max);
// (yotwo) Holds all downloading (the one in progress stops within a segment and keeps what it
// has) or lets it carry on.
void download_set_paused(bool paused);
// Changes whenever any download starts, progresses, finishes or is removed.
uint32_t download_generation(void);
// True while anything is queued, downloading or being removed.
bool download_busy(void);

/* For the player */

// Duration and chapters of a completed download (caller frees with abs_free_session).
esp_err_t download_load_meta(const char *item_id, abs_session_t *out);
// Full path of the audio file, and the byte offset where segment `seg` starts.
void download_audio_path(const char *item_id, char *path, size_t len);
esp_err_t download_segment_offset(const char *item_id, int seg, uint32_t *offset);

// Local listening position. `pending` means the server hasn't been told yet.
bool download_get_progress(const char *item_id, double *position, bool *pending);
void download_set_progress(const char *item_id, double position, bool pending);

// Overlays pending local positions onto freshly loaded books and, when online, pushes them to
// the server (clearing the pending flag on success).
void download_sync_progress(abs_book_t *books, int count, bool online);
