#include "favourites.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "config.h"
#include "wifi.h"

static const char *TAG = "favourites";

#define ID_LEN       40
#define MAX_FAVS     100
#define MAX_PENDING  32
#define COLLECTION   "Favourites"
#define RETRY_MS     60000

// A change not yet on the server.
typedef struct {
    char library[ID_LEN];
    char id[ID_LEN];
    uint8_t add;
} pending_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char s_favs[MAX_FAVS][ID_LEN];
static int s_fav_count;
static pending_t s_pending[MAX_PENDING];
static int s_pending_count;
static uint32_t s_generation;

// The library on show.
static char s_library[ID_LEN];
static bool s_podcast;
static char (*s_books)[ID_LEN];   // its item ids, to tell which favourites are this library's
static int s_book_count;
static bool s_need_pull;
static char s_collection[ID_LEN]; // the library's "Favourites" collection; "" if none (known after a pull)
static bool s_pulled;
static pending_t s_sending;       // the change being sent right now (id "" if none)

/* Storage. Call with the lock held; the write itself is handed to a task that may touch flash. */

static void write_nvs(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) != ESP_OK) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e1 = ESP_OK, e2 = ESP_OK;
    if (s_fav_count) e1 = nvs_set_blob(h, "favs", s_favs, (size_t)s_fav_count * ID_LEN);
    else nvs_erase_key(h, "favs");
    if (s_pending_count) e2 = nvs_set_blob(h, "favq", s_pending, (size_t)s_pending_count * sizeof(pending_t));
    else nvs_erase_key(h, "favq");
    xSemaphoreGive(s_lock);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGE(TAG, "saving favourites failed: %s / %s", esp_err_to_name(e1), esp_err_to_name(e2));
    }
    nvs_commit(h);
    nvs_close(h);
}

static void save(void)
{
    flash_safe(write_nvs, NULL);
}

static int slot_of(const char *id)
{
    for (int i = 0; i < s_fav_count; i++) {
        if (strcmp(s_favs[i], id) == 0) return i;
    }
    return -1;
}

static bool set_fav(const char *id, bool on)
{
    const int slot = slot_of(id);
    if (on && slot < 0) {
        if (s_fav_count >= MAX_FAVS) return false;
        strlcpy(s_favs[s_fav_count++], id, ID_LEN);
    } else if (!on && slot >= 0) {
        memmove(s_favs[slot], s_favs[slot + 1], (size_t)(s_fav_count - slot - 1) * ID_LEN);
        s_fav_count--;
    } else {
        return false;
    }
    s_generation++;
    return true;
}

static bool in_library(const char *id)
{
    for (int i = 0; i < s_book_count; i++) {
        if (strcmp(s_books[i], id) == 0) return true;
    }
    return false;
}

/* Reading the server's replies.

   Collections come back "expanded", with every book as a full library item, so a reply can
   be large. Rather than build a JSON tree of all that, this walks the text once and picks
   out the two things needed: for each object in the array called `array_key` at the top
   level, its own "id" and "name". */

typedef void (*element_cb)(const char *id, const char *name, void *ctx);

static const char *skip_string(const char *p)  // p at the opening quote; returns past the closing one
{
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
    }
    return *p ? p + 1 : p;
}

static void copy_string(const char *p, char *out, size_t len)  // p at the opening quote
{
    size_t n = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        if (n + 1 < len) out[n++] = *p;
    }
    out[n] = 0;
}

static void scan_array(const char *json, const char *array_key, element_cb cb, void *ctx)
{
    int depth = 0;
    // Depth 1 is inside the top-level object, 2 inside the wanted array, 3 inside one of
    // its elements; anything deeper belongs to an element's own contents and is skipped.
    bool in_array = false;
    char key[32] = "", id[ID_LEN] = "", name[64] = "";
    bool expect_value = false;
    for (const char *p = json; *p;) {
        const char c = *p;
        if (c == '"') {
            const char *end = skip_string(p);
            const char *q = end;
            while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
            if (*q == ':' && !expect_value) {
                // an object key
                if (depth == 1 || (in_array && depth == 3)) copy_string(p, key, sizeof(key));
                else key[0] = 0;
                expect_value = true;
                p = q + 1;
                continue;
            }
            if (in_array && depth == 3 && expect_value) {
                if (strcmp(key, "id") == 0) copy_string(p, id, sizeof(id));
                else if (strcmp(key, "name") == 0) copy_string(p, name, sizeof(name));
            }
            expect_value = false;
            p = end;
            continue;
        }
        if (c == '{' || c == '[') {
            if (c == '[' && depth == 1 && strcmp(key, array_key) == 0) in_array = true;
            if (c == '{' && in_array && depth == 2) id[0] = name[0] = 0;
            depth++;
            expect_value = false;
            key[0] = 0;
        } else if (c == '}' || c == ']') {
            depth--;
            if (c == '}' && in_array && depth == 2 && id[0]) cb(id, name, ctx);
            if (c == ']' && in_array && depth == 1) in_array = false;
            expect_value = false;
        } else if (c == ',') {
            expect_value = false;
        } else if (c != ' ' && c != '\n' && c != '\r' && c != '\t' && c != ':') {
            expect_value = false;  // a number, true, false or null
        }
        p++;
    }
}

static void find_collection(const char *id, const char *name, void *ctx)
{
    // Either spelling, so an existing "Favorites" is used rather than duplicated.
    char *out = ctx;
    if (!out[0] && (strcasecmp(name, COLLECTION) == 0 || strcasecmp(name, "Favorites") == 0)) {
        strlcpy(out, id, ID_LEN);
    }
}

typedef struct { char (*ids)[ID_LEN]; int count, max; } id_list_t;

static void collect_id(const char *id, const char *name, void *ctx)
{
    id_list_t *l = ctx;
    if (l->count < l->max) strlcpy(l->ids[l->count++], id, ID_LEN);
}

/* Talking to the server. These run on the sync task, without the lock held. */

// Reads the library's Favourites collection and makes the player agree with it, then
// re-applies whatever is still waiting to be sent. Returns false if the server could not
// be asked.
static bool pull(const char *library)
{
    char path[128], collection[ID_LEN] = "";
    char *body = NULL;
    snprintf(path, sizeof(path), "/api/libraries/%s/collections", library);
    int status = abs_api_call("GET", path, NULL, &body);
    if (status != 200 || !body) {
        free(body);
        ESP_LOGW(TAG, "could not list collections (%d)", status);
        return false;
    }
    scan_array(body, "results", find_collection, collection);
    free(body);

    id_list_t remote = { .ids = heap_caps_malloc(MAX_FAVS * ID_LEN, MALLOC_CAP_SPIRAM), .max = MAX_FAVS };
    if (!remote.ids) return false;
    if (collection[0]) {
        snprintf(path, sizeof(path), "/api/collections/%s", collection);
        status = abs_api_call("GET", path, NULL, &body);
        if (status != 200 || !body) {
            free(body);
            free(remote.ids);
            ESP_LOGW(TAG, "could not read the collection (%d)", status);
            return false;
        }
        scan_array(body, "books", collect_id, &remote);
        free(body);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(library, s_library) == 0) {
        strlcpy(s_collection, collection, sizeof(s_collection));
        s_pulled = true;
        if (collection[0]) {
            // This library's favourites become the collection's books...
            int kept = 0, dropped = 0, added = 0;
            for (int i = s_fav_count - 1; i >= 0; i--) {
                if (!in_library(s_favs[i])) continue;
                bool there = false;
                for (int k = 0; k < remote.count && !there; k++) there = strcmp(remote.ids[k], s_favs[i]) == 0;
                if (there) kept++;
                else { set_fav(s_favs[i], false); dropped++; }
            }
            for (int k = 0; k < remote.count; k++) {
                if (set_fav(remote.ids[k], true)) added++;
            }
            // ...and then what the player has done since and not yet sent still stands.
            for (int i = 0; i < s_pending_count; i++) {
                if (strcmp(s_pending[i].library, library) == 0) set_fav(s_pending[i].id, s_pending[i].add);
            }
            ESP_LOGI(TAG, "\"%s\" collection: %d books (%d new to the player, %d no longer favourites, %d unchanged)",
                     COLLECTION, remote.count, added, dropped, kept);
        } else {
            ESP_LOGI(TAG, "no \"%s\" collection in this library yet", COLLECTION);
        }
    }
    xSemaphoreGive(s_lock);
    free(remote.ids);
    save();
    return true;
}

// Creates the collection holding every favourite this library has on the player.
static int create_collection(const char *library, char *collection)
{
    char *body = heap_caps_malloc(160 + MAX_FAVS * (ID_LEN + 3), MALLOC_CAP_SPIRAM);
    if (!body) return -1;
    int n = sprintf(body, "{\"libraryId\":\"%s\",\"name\":\"%s\",\"books\":[", library, COLLECTION);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool first = true;
    for (int i = 0; i < s_fav_count; i++) {
        if (!in_library(s_favs[i])) continue;
        n += sprintf(body + n, "%s\"%s\"", first ? "" : ",", s_favs[i]);
        first = false;
    }
    xSemaphoreGive(s_lock);
    strcpy(body + n, "]}");
    char *reply = NULL;
    const int status = abs_api_call("POST", "/api/collections", body, &reply);
    free(body);
    if (status == 200 && reply) {
        // The reply is the collection; its id is the first "id" in it.
        const char *p = strstr(reply, "\"id\"");
        p = p ? strchr(p + 4, '"') : NULL;
        if (p) copy_string(p, collection, ID_LEN);
    }
    free(reply);
    return status;
}

// Sends one queued change. Returns true when it is dealt with (sent, or pointless to retry).
static bool push(const pending_t *op)
{
    char collection[ID_LEN], path[160], body[80];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(collection, s_collection, sizeof(collection));
    xSemaphoreGive(s_lock);

    int status;
    if (!collection[0]) {
        if (!op->add) return true;  // nothing to remove it from
        status = create_collection(op->library, collection);
        if (status == 200 && collection[0]) {
            ESP_LOGI(TAG, "created the \"%s\" collection with this library's favourites", COLLECTION);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (strcmp(op->library, s_library) == 0) strlcpy(s_collection, collection, sizeof(s_collection));
            xSemaphoreGive(s_lock);
            return true;
        }
    } else if (op->add) {
        snprintf(path, sizeof(path), "/api/collections/%s/book", collection);
        snprintf(body, sizeof(body), "{\"id\":\"%s\"}", op->id);
        status = abs_api_call("POST", path, body, NULL);
    } else {
        snprintf(path, sizeof(path), "/api/collections/%s/book/%s", collection, op->id);
        status = abs_api_call("DELETE", path, NULL, NULL);
    }
    if (status == 200) {
        ESP_LOGI(TAG, "%s %s the \"%s\" collection", op->id, op->add ? "added to" : "removed from", COLLECTION);
        return true;
    }
    if (status == 404 && collection[0]) {
        // The collection has gone (deleted on the server): look again, then retry.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_need_pull = true;
        xSemaphoreGive(s_lock);
        return false;
    }
    if (status == 403) {
        ESP_LOGW(TAG, "this account may not change collections; the favourite stays on the player only");
        return true;
    }
    if (status >= 400 && status < 500) {
        ESP_LOGW(TAG, "the server refused the change (%d); not retrying", status);
        return true;  // e.g. already in the collection
    }
    return false;  // out of reach or a server error: try again later
}

static void sync_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
        for (;;) {
            char library[ID_LEN];
            bool pull_now, have_op = false;
            pending_t op = { 0 };
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(library, s_library, sizeof(library));
            pull_now = s_need_pull;
            const bool usable = library[0] && !s_podcast;
            for (int i = 0; i < s_pending_count && !have_op && s_pulled; i++) {
                if (strcmp(s_pending[i].library, library) == 0) { op = s_pending[i]; have_op = true; }
            }
            xSemaphoreGive(s_lock);
            if (!usable || !wifi_is_connected() || (!pull_now && !have_op)) break;

            if (pull_now) {
                if (!pull(library)) break;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (strcmp(library, s_library) == 0) s_need_pull = false;
                xSemaphoreGive(s_lock);
                continue;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_sending = op;
            xSemaphoreGive(s_lock);
            const bool sent = push(&op);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memset(&s_sending, 0, sizeof(s_sending));
            xSemaphoreGive(s_lock);
            if (!sent) {
                bool again;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                again = s_need_pull;
                xSemaphoreGive(s_lock);
                if (again) continue;
                break;
            }
            // Sent: take it off the queue.
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_pending_count; i++) {
                if (memcmp(&s_pending[i], &op, sizeof(op)) == 0) {
                    memmove(&s_pending[i], &s_pending[i + 1], (size_t)(s_pending_count - i - 1) * sizeof(pending_t));
                    s_pending_count--;
                    break;
                }
            }
            xSemaphoreGive(s_lock);
            save();
        }
    }
}

/* Public */

void favourites_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_favs);
        if (nvs_get_blob(h, "favs", s_favs, &len) == ESP_OK) s_fav_count = (int)(len / ID_LEN);
        len = sizeof(s_pending);
        if (nvs_get_blob(h, "favq", s_pending, &len) == ESP_OK) s_pending_count = (int)(len / sizeof(pending_t));
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%d favourites on the player, %d changes waiting to be sent", s_fav_count, s_pending_count);
    // PSRAM stack: the TLS handshake needs the room (and so NVS writes go through flash_safe).
    // On core 1, for the same reason as the cover loader: core 0 is the busy one.
    xTaskCreatePinnedToCoreWithCaps(sync_task, "favsync", 8192, NULL, 1, &s_task, 1, MALLOC_CAP_SPIRAM);
}

int favourites_ids(char (*out)[40], int max, bool loaded_books_only)
{
    if (!s_lock) return 0;
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_fav_count && n < max; i++) {
        if (loaded_books_only && (s_podcast || !in_library(s_favs[i]))) continue;
        strlcpy(out[n++], s_favs[i], ID_LEN);
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool favourites_has(const char *item_id)
{
    if (!s_lock || !item_id) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool has = slot_of(item_id) >= 0;
    xSemaphoreGive(s_lock);
    return has;
}

void favourites_toggle(const char *item_id)
{
    if (!s_lock || !item_id || !item_id[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool on = slot_of(item_id) < 0;
    if (set_fav(item_id, on) && s_library[0] && !s_podcast) {
        // Queue it for the server; a change that undoes one still waiting cancels it instead.
        int undo = -1;
        for (int i = 0; i < s_pending_count; i++) {
            if (strcmp(s_pending[i].id, item_id) == 0 && strcmp(s_pending[i].library, s_library) == 0) undo = i;
        }
        // ...unless that one is already on its way: then this has to follow it.
        const bool in_flight = undo >= 0 && memcmp(&s_pending[undo], &s_sending, sizeof(pending_t)) == 0;
        if (in_flight) undo = -1;
        if (undo >= 0 && s_pending[undo].add != on) {
            memmove(&s_pending[undo], &s_pending[undo + 1], (size_t)(s_pending_count - undo - 1) * sizeof(pending_t));
            s_pending_count--;
        } else if (undo < 0 && s_pending_count < MAX_PENDING) {
            pending_t *p = &s_pending[s_pending_count++];
            memset(p, 0, sizeof(*p));
            strlcpy(p->library, s_library, ID_LEN);
            strlcpy(p->id, item_id, ID_LEN);
            p->add = on;
        }
    }
    xSemaphoreGive(s_lock);
    save();
    xTaskNotifyGive(s_task);
}

void favourites_set_library(const char *library_id, bool podcast, const abs_book_t *books, int count)
{
    if (!s_lock || !library_id) return;
    char (*ids)[ID_LEN] = count > 0 ? heap_caps_malloc((size_t)count * ID_LEN, MALLOC_CAP_SPIRAM) : NULL;
    for (int i = 0; ids && i < count; i++) strlcpy(ids[i], books[i].id, ID_LEN);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(s_library, library_id) != 0) {
        strlcpy(s_library, library_id, sizeof(s_library));
        s_collection[0] = 0;
        s_pulled = false;
    }
    s_podcast = podcast;
    free(s_books);
    s_books = ids;
    s_book_count = ids ? count : 0;
    s_need_pull = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

uint32_t favourites_generation(void)
{
    return s_generation;
}
