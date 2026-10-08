#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "abs_api.h"

typedef enum {
    PLAYER_IDLE,
    PLAYER_LOADING,     // starting a session
    PLAYER_BUFFERING,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_FINISHED,
    PLAYER_ERROR,
} player_state_t;

typedef struct {
    player_state_t state;
    char item_id[40];
    char episode_id[40];    // set when playing a podcast episode
    char title[128];
    char author[96];
    char chapter[96];
    int chapter_index;      // -1 if the book has no chapters
    int chapter_count;
    double position;        // seconds into the book
    double duration;
    double chapter_start;
    double chapter_end;
    int volume;             // 0-100
    int buffer_percent;
} player_status_t;

void player_init(void);

// All of these queue a command and return immediately.
// Plays a book; for a podcast show, resumes its most recent in-progress episode (if any).
void player_open(const abs_book_t *book);
// Plays one podcast episode. The title/author shown are replaced by the server's once it opens.
void player_open_episode(const char *item_id, const char *episode_id, const char *title, const char *show);
void player_toggle(void);
void player_seek_relative(double seconds);
void player_seek_to(double seconds);
void player_chapter_step(int delta);
void player_stop(void);
void player_set_volume(int volume);

void player_get_status(player_status_t *out);
// Title and time range (seconds) of chapter `index` of the open book; any output may be NULL.
// False if nothing is open or there is no such chapter.
bool player_get_chapter(int index, char *title, size_t len, double *start, double *end);
