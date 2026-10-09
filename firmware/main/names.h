#pragma once
// Titles and authors of books the loaded library does not hold: favourites and card books
// from other libraries, shown on the web page's downloads list. Looked up on the server one
// at a time in the background, and kept on the SD card so they show offline too.
#include <stdbool.h>
#include <stddef.h>

void names_init(void);
// The names if known (true). Otherwise false, and the book is queued to be looked up.
bool names_get(const char *item_id, char *title, size_t tlen, char *author, size_t alen);
// Looks up one queued book, if any (blocks for one server call). From a task that may use
// the network; does nothing while offline.
void names_work(void);
