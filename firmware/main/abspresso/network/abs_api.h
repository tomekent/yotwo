#pragma once

// Minimal Audiobookshelf REST client. All calls block; call them off the UI thread.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char id[40];
    char *title;
    char *sort_title;      // title without a leading "The"/"A" (server-provided), for A-Z
    char *author;          // may list several authors separated by ", "
    char *series;          // e.g. "Discworld #3" (may list several, ", "-separated); "" if none
    double duration;
    double current_time;   // saved progress, seconds
    float progress;        // 0..1
    bool finished;
    double last_update;    // progress timestamp (ms), 0 if never played
    double added_at;       // when the item was added to the library (ms)
    // Podcast libraries: each item is a show. Progress fields then describe the most recently
    // played unfinished episode, which resume_episode names.
    bool podcast;
    int num_episodes;
    char resume_episode[40];
} abs_book_t;

typedef struct {
    char id[40];
    char name[64];
    bool podcast;
} abs_library_t;

typedef struct {
    char id[40];
    char *title;
    double duration;
    double published_at;   // ms
    double current_time;
    float progress;
    bool finished;
    double last_update;
} abs_episode_t;

typedef struct {
    char *title;
    double start;
    double end;
} abs_chapter_t;

typedef struct {
    char id[40];
    char hls_path[96];     // e.g. /hls/<session>/output.m3u8
    double current_time;
    double duration;
    abs_chapter_t *chapters;
    int chapter_count;
    char display_title[128];   // episode title for podcasts
    char display_author[96];   // show title for podcasts
} abs_session_t;

// Uses the server and credential from config.h.
void abs_api_init(void);

// Signing in (for the setup portal, against a server that isn't saved yet).
typedef enum { ABS_AUTH_OK, ABS_AUTH_UNREACHABLE, ABS_AUTH_REJECTED, ABS_AUTH_ERROR } abs_auth_result_t;
// Username/password -> access token (+ refresh token on servers that issue them).
abs_auth_result_t abs_login(const char *base, const char *username, const char *password, char *access,
                            size_t alen, char *refresh, size_t rlen);
// Whether `token` (an API key or access token) is accepted.
abs_auth_result_t abs_check(const char *base, const char *token);
// True once the server rejected our credential and it couldn't be renewed.
bool abs_signed_out(void);

// All libraries on the server (books and podcasts), plus the raw JSON for caching.
esp_err_t abs_get_libraries(abs_library_t **out, int *count, char **json);
esp_err_t abs_parse_libraries(const char *json, abs_library_t **out, int *count);

// Items of one library, sorted A-Z by sort_title. Also returns the raw item and user JSON
// (caller frees) so they can be cached and re-parsed offline.
esp_err_t abs_get_books(const char *library_id, abs_book_t **out_books, int *out_count, char **items_json,
                        char **me_json);
// Name of the library being shown (for display), and the server host.
const char *abs_library_name(void);
void abs_set_library_name(const char *name);
const char *abs_server(void);

// A podcast's episodes with the user's progress: unfinished in-progress ones first (most recent
// first), then newest published.
esp_err_t abs_get_episodes(const char *item_id, abs_episode_t **out, int *count);
void abs_free_episodes(abs_episode_t *eps, int count);
// Compact JSON for caching episodes offline (caller frees), and back.
char *abs_episodes_to_json(const abs_episode_t *eps, int count);
esp_err_t abs_episodes_from_json(const char *json, abs_episode_t **out, int *count);
esp_err_t abs_parse_books(const char *items_json, const char *me_json, abs_book_t **out_books, int *out_count);
void abs_free_books(abs_book_t *books, int count);

// A JSON call to any endpoint, for features built outside this file. `method` is "GET",
// "POST" or "DELETE"; `body` may be NULL. Returns the HTTP status (<0 if it could not be
// made); the response body, if wanted, is returned in *out (PSRAM, NUL-terminated, caller
// frees). Blocks; opens its own connection, so any task may call it.
int abs_api_call(const char *method, const char *path, const char *body, char **out);

// Starts an HLS (transcoded) playback session at the saved position. episode_id is NULL/"" for
// books, or the episode to play for podcasts.
esp_err_t abs_start_session(const char *item_id, const char *episode_id, bool for_download, abs_session_t *out);
void abs_free_session(abs_session_t *s);
esp_err_t abs_sync_session(const char *session_id, double current_time, double time_listening, double duration);
esp_err_t abs_close_session(const char *session_id, double current_time, double time_listening);
// Sets progress directly, without a playback session (used for downloaded books).
// (yotwo) Where the user has got to in an item, from the server. ESP_ERR_NOT_FOUND if it has
// never been started; ESP_FAIL if the server could not be asked.
esp_err_t abs_get_progress(const char *item_id, double *current_time);
esp_err_t abs_patch_progress(const char *item_id, double current_time, double duration, bool finished);

// Downloads the item's cover as a JPEG scaled to `width` px wide. Caller frees *out.
// Keeps its connection open between calls; call from a single task only.
esp_err_t abs_get_cover(const char *item_id, int width, uint8_t **out, size_t *out_len);

// Streaming GETs for HLS segments over a keep-alive connection. Each user (player, downloader)
// owns its own stream; a stream must only be used from one task.
typedef struct abs_stream abs_stream_t;
abs_stream_t *abs_stream_create(void);
// Returns the HTTP status (or <0 on error). On 200 read with abs_stream_read(), then abs_stream_end().
int abs_stream_begin(abs_stream_t *st, const char *path);
int abs_stream_read(abs_stream_t *st, char *buf, int len);
bool abs_stream_complete(abs_stream_t *st);
void abs_stream_end(abs_stream_t *st, bool keep_alive);
