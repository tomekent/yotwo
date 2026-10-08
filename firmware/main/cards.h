#pragma once
// What each NFC card means. A card is known by its UID alone (nothing is written to it,
// so original Yoto cards and any other tag work). The records live in one small file on
// the SD card, STORAGE_ROOT/cards.json, which can be read, backed up or edited by hand.
//
// Two kinds so far: a book card plays a library item; a sleep card starts sleep mode.
#include <stdbool.h>
#include <stddef.h>

typedef enum { CARD_UNKNOWN, CARD_BOOK, CARD_SLEEP } card_kind_t;

typedef struct {
    char uid[16];        // hex, e.g. "045D9FFA972091"
    card_kind_t kind;
    // CARD_BOOK: enough to show and play the item without its library being loaded.
    char item[40];
    char title[128];
    char author[96];
    bool podcast;        // the item is a show (its library must be loaded to play it)
    // CARD_SLEEP
    int minutes;
} card_t;

// Reads the file. Call after storage_init(). Without an SD card, records are kept in
// memory only and are lost at power-off.
void cards_init(void);
bool cards_find(const char *uid, card_t *out);
// Adds or replaces the record for card->uid and saves.
void cards_put(const card_t *card);
void cards_remove(const char *uid);
// Writes the records to the card again (after it has been formatted).
void cards_resave(void);
// Copies out every record, up to `max`.
int cards_list(card_t *out, int max);
// Copies out the item ids of the books that cards play (not podcast shows), up to `max`.
int cards_books(char (*out)[40], int max);
// The title and author recorded for a card's book; false if no card plays that item.
bool cards_names(const char *item, char *title, size_t tlen, char *author, size_t alen);
