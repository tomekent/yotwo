#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "download.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "nvs.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "catalog";

#define LIBRARIES_JSON STORAGE_ROOT "/libraries.json"
#define ME_JSON        STORAGE_ROOT "/me.json"

static char s_library[40];
static abs_library_t *s_libs;
static int s_lib_count;
static abs_book_t *s_books;
static int s_count;

/* ---------- selection ---------- */

static void save_selection(void)
{
    nvs_handle_t h;
    if (nvs_open("abs", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "library", s_library);
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *catalog_library_id(void)
{
    return s_library;
}

static const abs_library_t *current(void)
{
    for (int i = 0; i < s_lib_count; i++) {
        if (strcmp(s_libs[i].id, s_library) == 0) return &s_libs[i];
    }
    return NULL;
}

bool catalog_library_is_podcast(void)
{
    const abs_library_t *l = current();
    return l && l->podcast;
}

void catalog_dir(char *out, size_t len)
{
    if (!storage_ready() || !s_library[0]) {
        out[0] = 0;
        return;
    }
    snprintf(out, len, STORAGE_ROOT "/lib/%s", s_library);
}

// Picks a library if none is selected (or the selected one is gone): the first book library.
static void ensure_selection(void)
{
    if (current()) return;
    for (int i = 0; i < s_lib_count; i++) {
        if (!s_libs[i].podcast || i == s_lib_count - 1) {
            strlcpy(s_library, s_libs[i].id, sizeof(s_library));
            save_selection();
            return;
        }
    }
}

static void set_libraries(abs_library_t *libs, int n)
{
    free(s_libs);
    s_libs = libs;
    s_lib_count = n;
    ESP_LOGI(TAG, "%d librar%s on the server", n, n == 1 ? "y" : "ies");
    ensure_selection();
    const abs_library_t *l = current();
    abs_set_library_name(l ? l->name : "");
    lvgl_port_lock(0);
    ui_set_libraries(s_libs, s_lib_count, s_library);
    lvgl_port_unlock();
}

void catalog_select(const char *library_id)
{
    strlcpy(s_library, library_id, sizeof(s_library));
    save_selection();
    const abs_library_t *l = current();
    abs_set_library_name(l ? l->name : "");
    lvgl_port_lock(0);
    ui_set_libraries(s_libs, s_lib_count, s_library);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "selected library %s", l ? l->name : library_id);
}

/* ---------- cache ---------- */

// Before per-library caches, items.json and covers/ sat directly in STORAGE_ROOT. Move them into
// the directory of the library they came from (named in the items themselves).
static void migrate(void)
{
    const char *old_items = STORAGE_ROOT "/items.json";
    struct stat st;
    if (stat(old_items, &st) != 0) return;
    char *json = storage_read_file(old_items, NULL);
    const char *p = json ? strstr(json, "\"libraryId\":\"") : NULL;
    char id[40] = "";
    if (p) sscanf(p + 13, "%39[^\"]", id);
    free(json);
    if (id[0]) {
        char dir[128], dst[160];
        snprintf(dir, sizeof(dir), STORAGE_ROOT "/lib/%s", id);
        storage_mkdirs(dir);
        snprintf(dst, sizeof(dst), "%s/items.json", dir);
        rename(old_items, dst);
        snprintf(dst, sizeof(dst), "%s/covers", dir);
        rename(STORAGE_ROOT "/covers", dst);
        if (!s_library[0]) {
            strlcpy(s_library, id, sizeof(s_library));
            save_selection();
        }
        ESP_LOGI(TAG, "moved the old cache into library %.8s", id);
    } else {
        unlink(old_items);
    }
    unlink(STORAGE_ROOT "/library.txt");
}

void catalog_init(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_library);
    if (nvs_open("abs", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "library", s_library, &len) != ESP_OK) s_library[0] = 0;
        nvs_close(h);
    }
    if (!storage_ready()) return;
    migrate();
    char *json = storage_read_file(LIBRARIES_JSON, NULL);
    abs_library_t *libs = NULL;
    int n = 0;
    if (json && abs_parse_libraries(json, &libs, &n) == ESP_OK && n > 0) set_libraries(libs, n);
    else free(libs);
    free(json);
}

static void show(abs_book_t *fresh, int n, bool cached)
{
    lvgl_port_lock(0);
    ui_set_books(fresh, n);
    ui_set_source(cached);
    lvgl_port_unlock();
    abs_free_books(s_books, s_count);
    s_books = fresh;
    s_count = n;
}

bool catalog_load_cached(void)
{
    char dir[96], path[128];
    catalog_dir(dir, sizeof(dir));
    if (!dir[0]) return false;
    snprintf(path, sizeof(path), "%s/items.json", dir);
    char *items = storage_read_file(path, NULL);
    char *me = storage_read_file(ME_JSON, NULL);
    abs_book_t *fresh = NULL;
    int n = 0;
    bool ok = items && abs_parse_books(items, me, &fresh, &n) == ESP_OK;
    free(items);
    free(me);
    if (!ok) {
        abs_free_books(fresh, n);
        return false;
    }
    download_sync_progress(fresh, n, false);
    ESP_LOGI(TAG, "showing %d items of %s from the SD cache", n, abs_library_name());
    show(fresh, n, true);
    return true;
}

bool catalog_load_network(bool quiet)
{
    char *json = NULL;
    abs_library_t *libs = NULL;
    int nl = 0;
    if (abs_get_libraries(&libs, &nl, &json) != ESP_OK || nl == 0) {
        free(json);
        free(libs);
        if (!quiet) ui_show_message_locked("Couldn't reach Audiobookshelf.\nTry Refresh in Settings.");
        return false;
    }
    if (storage_ready()) storage_write_file(LIBRARIES_JSON, json, strlen(json));
    free(json);
    set_libraries(libs, nl);

    abs_book_t *fresh = NULL;
    int n = 0;
    char *items = NULL, *me = NULL;
    if (abs_get_books(s_library, &fresh, &n, &items, &me) != ESP_OK) {
        if (!quiet) ui_show_message_locked("Couldn't load the library.\nTry Refresh in Settings.");
        return false;
    }
    char dir[96];
    catalog_dir(dir, sizeof(dir));
    if (dir[0]) {
        char path[128];
        storage_mkdirs(dir);
        snprintf(path, sizeof(path), "%s/items.json", dir);
        storage_write_file(path, items, strlen(items));
        storage_write_file(ME_JSON, me, strlen(me));
    }
    free(items);
    free(me);
    download_sync_progress(fresh, n, true);
    show(fresh, n, false);
    return true;
}
