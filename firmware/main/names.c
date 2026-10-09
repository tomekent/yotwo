#include "names.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "abs_api.h"
#include "account.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage.h"
#include "wifi.h"

static const char *TAG = "names";

#define FILE_PATH STORAGE_ROOT "/names.txt"  // id <tab> title <tab> author, a line each
#define MAX_NAMES 64
#define MAX_QUEUE 16

typedef struct { char id[40], title[96], author[64]; } entry_t;

static entry_t *s_names;
static int s_count;
static char (*s_queue)[40];
static int s_queued;
static SemaphoreHandle_t s_lock;

static int find(const char *id)  // with the lock held
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_names[i].id, id) == 0) return i;
    }
    return -1;
}

void names_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_names = heap_caps_calloc(MAX_NAMES, sizeof(entry_t), MALLOC_CAP_SPIRAM);
    s_queue = heap_caps_calloc(MAX_QUEUE, 40, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_names || !s_queue) return;
    char *text = storage_read_file(FILE_PATH, NULL);
    char *save = NULL;
    for (char *line = text ? strtok_r(text, "\n", &save) : NULL; line && s_count < MAX_NAMES;
         line = strtok_r(NULL, "\n", &save)) {
        char *t = strchr(line, '\t'), *a = t ? strchr(t + 1, '\t') : NULL;
        if (!t || !a || t - line >= 40) continue;
        *t = *a = 0;
        entry_t *e = &s_names[s_count++];
        strlcpy(e->id, line, sizeof(e->id));
        strlcpy(e->title, t + 1, sizeof(e->title));
        strlcpy(e->author, a + 1, sizeof(e->author));
    }
    free(text);
    if (s_count) ESP_LOGI(TAG, "%d names of books outside the library", s_count);
}

bool names_get(const char *item_id, char *title, size_t tlen, char *author, size_t alen)
{
    if (!s_lock || !item_id[0] || strlen(item_id) >= 40) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int i = find(item_id);
    if (i >= 0) {
        strlcpy(title, s_names[i].title, tlen);
        strlcpy(author, s_names[i].author, alen);
    } else {
        bool queued = false;
        for (int k = 0; k < s_queued && !queued; k++) queued = strcmp(s_queue[k], item_id) == 0;
        if (!queued && s_queued < MAX_QUEUE) strlcpy(s_queue[s_queued++], item_id, 40);
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
}

static void save(void)  // with the lock held
{
    char *text = heap_caps_malloc((size_t)s_count * sizeof(entry_t) + 1, MALLOC_CAP_SPIRAM);
    if (!text) return;
    size_t n = 0;
    for (int i = 0; i < s_count; i++) {
        n += (size_t)sprintf(text + n, "%s\t%s\t%s\n", s_names[i].id, s_names[i].title, s_names[i].author);
    }
    storage_write_file(FILE_PATH, text, n);
    free(text);
}

// Names off a tab or newline, which would break the file's lines.
static void clean(char *s)
{
    for (; *s; s++) {
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
    }
}

void names_work(void)
{
    if (!s_lock || !wifi_is_connected() || !account_present()) return;
    char id[40];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool any = s_queued > 0;
    if (any) {
        strlcpy(id, s_queue[0], sizeof(id));
        memmove(s_queue[0], s_queue[1], (size_t)(--s_queued) * 40);
    }
    xSemaphoreGive(s_lock);
    if (!any) return;

    char path[96], *body = NULL;
    snprintf(path, sizeof(path), "/api/items/%s", id);
    const int status = abs_api_call("GET", path, NULL, &body);
    entry_t e = { 0 };
    strlcpy(e.id, id, sizeof(e.id));
    if (status == 404) {
        strlcpy(e.title, "(no longer on the server)", sizeof(e.title));
    } else if (status == 200 && body) {
        cJSON *root = cJSON_Parse(body);
        const cJSON *meta = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "media"), "metadata");
        const cJSON *title = cJSON_GetObjectItem(meta, "title");
        if (cJSON_IsString(title)) strlcpy(e.title, title->valuestring, sizeof(e.title));
        // A book has authors (a list, or authorName in the short form); a podcast, author.
        const cJSON *name = cJSON_GetObjectItem(meta, "authorName");
        if (!cJSON_IsString(name)) name = cJSON_GetObjectItem(meta, "author");
        if (cJSON_IsString(name)) {
            strlcpy(e.author, name->valuestring, sizeof(e.author));
        } else {
            const cJSON *a;
            cJSON_ArrayForEach(a, cJSON_GetObjectItem(meta, "authors")) {
                const cJSON *n = cJSON_GetObjectItem(a, "name");
                if (!cJSON_IsString(n)) continue;
                if (e.author[0]) strlcat(e.author, ", ", sizeof(e.author));
                strlcat(e.author, n->valuestring, sizeof(e.author));
            }
        }
        cJSON_Delete(root);
    }
    free(body);
    if (status >= 0 && !e.title[0]) strlcpy(e.title, "(unknown book)", sizeof(e.title));  // answered, but not usefully
    if (!e.title[0]) {  // the server could not be reached: try again later, behind any others
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_queued < MAX_QUEUE) strlcpy(s_queue[s_queued++], id, 40);
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "%.8s: no name from the server (status %d)", id, status);
        return;
    }
    clean(e.title);
    clean(e.author);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (find(id) < 0) {
        if (s_count == MAX_NAMES) memmove(&s_names[0], &s_names[1], (size_t)(--s_count) * sizeof(entry_t));
        s_names[s_count++] = e;
        save();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%.8s is \"%s\" by %s", id, e.title, e.author[0] ? e.author : "(no author)");
}
