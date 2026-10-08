#include "cards.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "storage.h"

static const char *TAG = "cards";

#define PATH STORAGE_ROOT "/cards.json"
#define MAX_CARDS 64

static card_t s_cards[MAX_CARDS];
static int s_count;
// The records are used from the UI task, the offline task and the web server's.
static SemaphoreHandle_t s_lock;
#define LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

void cards_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_count = 0;
    char *text = storage_ready() ? storage_read_file(PATH, NULL) : NULL;
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    const cJSON *o;
    cJSON_ArrayForEach(o, root) {
        if (s_count >= MAX_CARDS) break;
        card_t *c = &s_cards[s_count];
        memset(c, 0, sizeof(*c));
        strlcpy(c->uid, str(o, "uid"), sizeof(c->uid));
        const char *kind = str(o, "kind");
        c->kind = strcmp(kind, "book") == 0 ? CARD_BOOK : strcmp(kind, "sleep") == 0 ? CARD_SLEEP : CARD_UNKNOWN;
        if (!c->uid[0] || c->kind == CARD_UNKNOWN) continue;
        strlcpy(c->item, str(o, "item"), sizeof(c->item));
        strlcpy(c->title, str(o, "title"), sizeof(c->title));
        strlcpy(c->author, str(o, "author"), sizeof(c->author));
        c->podcast = cJSON_IsTrue(cJSON_GetObjectItem(o, "podcast"));
        const cJSON *m = cJSON_GetObjectItem(o, "minutes");
        c->minutes = cJSON_IsNumber(m) ? m->valueint : 30;
        s_count++;
    }
    cJSON_Delete(root);
    free(text);
    ESP_LOGI(TAG, "%d cards known%s", s_count, storage_ready() ? "" : " (no SD card: they will not be remembered)");
}

static void save(void)
{
    if (!storage_ready()) return;
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < s_count; i++) {
        const card_t *c = &s_cards[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "uid", c->uid);
        if (c->kind == CARD_BOOK) {
            cJSON_AddStringToObject(o, "kind", "book");
            cJSON_AddStringToObject(o, "item", c->item);
            cJSON_AddStringToObject(o, "title", c->title);
            cJSON_AddStringToObject(o, "author", c->author);
            if (c->podcast) cJSON_AddTrueToObject(o, "podcast");
        } else {
            cJSON_AddStringToObject(o, "kind", "sleep");
            cJSON_AddNumberToObject(o, "minutes", c->minutes);
        }
        cJSON_AddItemToArray(root, o);
    }
    char *text = cJSON_Print(root);
    if (text) storage_write_file(PATH, text, strlen(text));
    cJSON_free(text);
    cJSON_Delete(root);
}

static int slot_of(const char *uid)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_cards[i].uid, uid) == 0) return i;
    }
    return -1;
}

bool cards_find(const char *uid, card_t *out)
{
    LOCK();
    const int i = uid && uid[0] ? slot_of(uid) : -1;
    if (i >= 0) *out = s_cards[i];
    UNLOCK();
    return i >= 0;
}

void cards_put(const card_t *card)
{
    LOCK();
    int i = slot_of(card->uid);
    if (i < 0 && s_count >= MAX_CARDS) {
        ESP_LOGW(TAG, "no room for another card (%d)", MAX_CARDS);
    } else {
        if (i < 0) i = s_count++;
        s_cards[i] = *card;
        save();
    }
    UNLOCK();
}

void cards_remove(const char *uid)
{
    LOCK();
    const int i = slot_of(uid);
    if (i >= 0) {
        memmove(&s_cards[i], &s_cards[i + 1], (size_t)(s_count - i - 1) * sizeof(card_t));
        s_count--;
        save();
    }
    UNLOCK();
}

int cards_books(char (*out)[40], int max)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < s_count && n < max; i++) {
        if (s_cards[i].kind == CARD_BOOK && !s_cards[i].podcast && s_cards[i].item[0]) {
            strlcpy(out[n], s_cards[i].item, 40);
            out[n++][39] = 0;
        }
    }
    UNLOCK();
    return n;
}

bool cards_names(const char *item, char *title, size_t tlen, char *author, size_t alen)
{
    LOCK();
    for (int i = 0; i < s_count; i++) {
        if (s_cards[i].kind == CARD_BOOK && strcmp(s_cards[i].item, item) == 0) {
            strlcpy(title, s_cards[i].title, tlen);
            strlcpy(author, s_cards[i].author, alen);
            UNLOCK();
            return true;
        }
    }
    UNLOCK();
    return false;
}

int cards_list(card_t *out, int max)
{
    LOCK();
    const int n = s_count < max ? s_count : max;
    memcpy(out, s_cards, (size_t)n * sizeof(card_t));
    UNLOCK();
    return n;
}

void cards_resave(void)
{
    LOCK();
    save();
    UNLOCK();
}

