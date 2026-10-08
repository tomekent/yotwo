#pragma once
// Books kept on the SD card for playing without the server: the favourites and the books
// that cards are linked to, and nothing else.
//
// A download is the server's transcode of the whole book (ABSpresso's download.c), so each
// one puts the server to work. They are therefore taken one at a time, in the background,
// and only while nothing is playing, the player is on external power and Wi-Fi is up; they
// stop within a few seconds when any of those ceases and carry on from where they were
// when all hold again. A book that stops being a favourite and has no card loses its copy.
#include <stdbool.h>

// Call after download_init(), favourites_init() and cards_init().
void offline_init(void);

// A favourite or card book can be told not to keep a copy (from the player's web page):
// its copy is removed and it is not fetched again until told otherwise, or until it stops
// being a favourite or a card's book. The list is STORAGE_ROOT/offline_skip.txt.
void offline_skip(const char *item_id, bool skip);
bool offline_skipped(const char *item_id);
// Whether a book is a favourite or a card's book (and so would be kept).
bool offline_is_wanted(const char *item_id);
// Everything that would be kept (favourites and card books), skipped or not, up to `max`.
int offline_wanted(char (*out)[40], int max);

typedef enum { OFFLINE_NO, OFFLINE_PARTLY, OFFLINE_YES } offline_state_t;
// Whether a book's copy is on the card: not at all, wanted or part-way, or complete.
offline_state_t offline_state(const char *item_id);
