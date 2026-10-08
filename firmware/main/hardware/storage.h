#pragma once

// SD card (FAT) mounted at /sdcard, used for caching and downloads. Everything degrades
// gracefully without a card: callers check storage_ready().

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define STORAGE_ROOT "/sdcard/abs"

esp_err_t storage_init(void);

// SD card calls from a task whose stack is in PSRAM are run on a helper task with an
// internal stack, and waited for. Twice (2026-10-08) such a task crashed inside the SD
// driver with a value on its stack reading back wrong, the fault this revision of the ESP32
// has with PSRAM; the driver keeps its command buffer on the caller's stack and is deep.
// The functions below do this themselves. For anything else that touches the card
// (stat, opendir, fopen, fread ...) from such a task, wrap it in a function and pass it here.
// From a task with an internal stack the function is simply called.
void storage_call(void (*fn)(void *), void *arg);
// Makes a fresh, empty FAT32 file system on the card, losing everything on it. Works on a
// card in use and on one that could not be read (blank, or exFAT). The caller should have
// stopped playback and downloads, and should restart the player afterwards: everything
// that had files open or lists in memory is then out of date.
esp_err_t storage_format(void);
// A file's size in bytes; -1 if it is not there.
long storage_file_size(const char *path);
bool storage_ready(void);
// Free and total space in bytes (0 if no card).
void storage_space(uint64_t *free_bytes, uint64_t *total_bytes);

// Whole-file helpers. read returns a NUL-terminated PSRAM buffer the caller frees.
char *storage_read_file(const char *path, size_t *len);
// Writes via a temporary file and rename, so a crash never leaves a half-written file.
esp_err_t storage_write_file(const char *path, const void *data, size_t len);
// Creates the directory and any missing parents.
esp_err_t storage_mkdirs(const char *path);
// Deletes a directory tree.
esp_err_t storage_remove_tree(const char *path);
