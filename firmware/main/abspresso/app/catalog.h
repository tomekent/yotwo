#pragma once

// Which library is shown, and loading it (from the SD cache or the server).
//
// The selected library id is kept in NVS. Each library has its own cache directory,
// STORAGE_ROOT/lib/<library id>/ (items.json, covers/, episodes/), so switching libraries never
// overwrites another library's cache. The user record (progress for everything) and the list of
// libraries are shared: STORAGE_ROOT/me.json and libraries.json.
//
// Loading functions block on the network; call them from the main task, not the UI.

#include <stdbool.h>
#include <stddef.h>
#include "abs_api.h"

void catalog_init(void);

// Currently selected library ("" until known) and its cache directory ("" without an SD card).
const char *catalog_library_id(void);
bool catalog_library_is_podcast(void);
void catalog_dir(char *out, size_t len);

// Shows the selected library from the SD cache. Returns false if there is none.
bool catalog_load_cached(void);
// Fetches the library list and the selected library's items from the server and shows them.
bool catalog_load_network(bool quiet);
// Makes another library the selected one (saved); the caller then loads it.
void catalog_select(const char *library_id);
