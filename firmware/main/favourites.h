#pragma once
// Favourite books.
//
// Kept on the player (NVS) so they work offline and at once, and mirrored to a collection
// called "Favourites" on the Audiobookshelf server, one per library (collections belong to
// a library). When a library is loaded and the server is in reach, the collection is read
// and replaces the player's idea of that library's favourites; if there is none, it is
// created the first time a favourite is added. Adding and removing update it. Changes made
// offline are queued (and survive a restart) and sent when the server is back.
//
// Collections hold books only, so favourites in a podcast library stay on the player.
#include <stdbool.h>
#include <stdint.h>
#include "abs_api.h"

void favourites_init(void);
bool favourites_has(const char *item_id);
// Marks or unmarks a book of the current library.
void favourites_toggle(const char *item_id);
// The current library and its books (call whenever they are loaded). Starts a sync.
void favourites_set_library(const char *library_id, bool podcast, const abs_book_t *books, int count);
// Copies out the favourites' item ids, up to `max`: all of them, in whichever library, or only
// those that are books of the library now loaded (none if that is a podcast library).
int favourites_ids(char (*out)[40], int max, bool loaded_books_only);
// Changes whenever the set changes, including from a sync.
uint32_t favourites_generation(void);
