#include "abs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "config.h"
#include "text.h"

static const char *TAG = "abs";

// The server's reverse proxy rejects some default user agents.
#define USER_AGENT "ABSpresso/0.1"
#define DEVICE_NAME "ABSpresso"

// "Bearer <token>", replaced when the access token is refreshed; s_auth_gen counts refreshes.
static char *s_auth;
static SemaphoreHandle_t s_auth_lock, s_refresh_lock;
static volatile uint32_t s_auth_gen;
static volatile bool s_signed_out;
static char s_base[128];  // server base URL, no trailing slash
static char s_device_id[24];
static char s_library_name[64];
static SemaphoreHandle_t s_sync_lock;           // see sync_request()
static esp_http_client_handle_t s_sync_conn;
static char s_device_id_dl[28];
struct abs_stream {
    esp_http_client_handle_t client;
};

// Library listings are hundreds of KB of JSON; keep cJSON's many small nodes out of internal RAM.
static void *psram_malloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

static char *psram_strdup(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *d = psram_malloc(n);
    if (d) memcpy(d, s ? s : "", n);
    return d;
}

void abs_api_init(void)
{
    s_sync_lock = xSemaphoreCreateMutex();
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    s_auth_lock = xSemaphoreCreateMutex();
    s_refresh_lock = xSemaphoreCreateMutex();
    s_auth = heap_caps_malloc(1100, MALLOC_CAP_SPIRAM);
    snprintf(s_auth, 1100, "Bearer %s", config_get()->access);
    strlcpy(s_base, config_get()->server, sizeof(s_base));
    size_t n = strlen(s_base);
    if (n && s_base[n - 1] == '/') s_base[n - 1] = 0;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_device_id_dl, sizeof(s_device_id_dl), "%s-dl", s_device_id);
}

static esp_http_client_handle_t new_client(const char *path, esp_http_client_method_t method)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = USER_AGENT,
        .keep_alive_enable = true,
    };
    return esp_http_client_init(&cfg);
}

// Sets the current Authorization header (tokens can change between requests on a kept connection).
static uint32_t set_auth(esp_http_client_handle_t c)
{
    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
    esp_http_client_set_header(c, "Authorization", s_auth);
    uint32_t gen = s_auth_gen;
    xSemaphoreGive(s_auth_lock);
    return gen;
}

static bool try_refresh(uint32_t failed_gen);
// (yotwo) The saved server password, if the firmware keeps one (setup/credential.c).
bool credential_load(char *out, size_t len) __attribute__((weak));

// Performs a request and returns the body (NUL-terminated, PSRAM) in *out and its length in *out_len.
// With `keep`, the connection is reused across calls (skipping a multi-second TLS handshake each
// time); *keep must only be used from one task. Returns HTTP status or -1.
static int request_once(esp_http_client_method_t method, const char *path, const char *body, char **out,
                        size_t *out_len, esp_http_client_handle_t *keep, uint32_t *gen)
{
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    const bool reused = keep && *keep;
    esp_http_client_handle_t c;
    if (reused) {
        char url[256];
        snprintf(url, sizeof(url), "%s%s", s_base, path);
        c = *keep;
        esp_http_client_set_url(c, url);
        esp_http_client_set_method(c, method);
    } else {
        c = new_client(path, method);
        if (!c) return -1;
    }
    *gen = set_auth(c);
    int body_len = body ? strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    int status = -1;
    bool complete = false;
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: connect failed: %s", path, esp_err_to_name(err));
        goto done;
    }
    if (body_len && esp_http_client_write(c, body, body_len) != body_len) {
        ESP_LOGE(TAG, "%s: write failed", path);
        goto done;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        goto done;
    }
    status = esp_http_client_get_status_code(c);

    size_t cap = 16 * 1024, len = 0;
    char *buf = psram_malloc(cap);
    while (buf) {
        if (cap - len < 4097) {
            char *nb = heap_caps_realloc(buf, cap * 2, MALLOC_CAP_SPIRAM);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        int n = esp_http_client_read(c, buf + len, 4096);
        if (n <= 0) break;
        len += n;
    }
    complete = esp_http_client_is_complete_data_received(c);
    if (buf && status == 200 && !complete) {
        // (yotwo) The connection failed part-way through the reply. What arrived is not the
        // answer: a collection read this way came back as "no books", the favourites were
        // dropped for it and their downloaded copies deleted (2026-10-08).
        ESP_LOGW(TAG, "%s: reply cut short after %u bytes; discarded", path, (unsigned)len);
        free(buf);
        buf = NULL;
        status = -1;
        goto done;
    }
    if (buf) {
        buf[len] = 0;
        if (out_len) *out_len = len;
        if (out) *out = buf; else free(buf);
    } else {
        ESP_LOGE(TAG, "%s: out of memory reading response", path);
        status = -1;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "%s %s -> %d", method == HTTP_METHOD_GET ? "GET" : (method == HTTP_METHOD_POST ? "POST" : "PATCH"),
                 path, status);
    }
done:
    if (keep && status > 0 && complete) {
        *keep = c;
        return status;
    }
    esp_http_client_cleanup(c);
    if (keep) *keep = NULL;
    if (reused && status < 0) {
        // The server probably dropped the idle connection; try once on a fresh one.
        return request_once(method, path, body, out, out_len, keep, gen);
    }
    return status;
}

// A request, retried once with a renewed access token if the server says the token has expired.
static int request_len(esp_http_client_method_t method, const char *path, const char *body, char **out,
                       size_t *out_len, esp_http_client_handle_t *keep)
{
    uint32_t gen = 0;
    int status = request_once(method, path, body, out, out_len, keep, &gen);
    if (status == 401 && try_refresh(gen)) {
        if (out) free(*out);
        status = request_once(method, path, body, out, out_len, keep, &gen);
    }
    return status;
}

static int request(esp_http_client_method_t method, const char *path, const char *body, char **out)
{
    return request_len(method, path, body, out, NULL, NULL);
}

// Progress sync, session close and progress PATCH are small and frequent: they share one kept-open
// connection (saving a multi-second TLS handshake each time), serialised because several tasks
// may call them.
// (yotwo)
esp_err_t abs_get_progress(const char *item_id, double *current_time)
{
    char path[96], *body = NULL;
    snprintf(path, sizeof(path), "/api/me/progress/%s", item_id);
    const int status = request(HTTP_METHOD_GET, path, NULL, &body);
    esp_err_t err = status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    const char *p = status == 200 && body ? strstr(body, "\"currentTime\":") : NULL;
    if (p) {
        *current_time = strtod(p + 14, NULL);
        err = ESP_OK;
    }
    free(body);
    return err;
}

int abs_api_call(const char *method, const char *path, const char *body, char **out)
{
    const esp_http_client_method_t m = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST
                                     : strcmp(method, "DELETE") == 0 ? HTTP_METHOD_DELETE : HTTP_METHOD_GET;
    return request(m, path, body, out);
}

static int sync_request(esp_http_client_method_t method, const char *path, const char *body)
{
    xSemaphoreTake(s_sync_lock, portMAX_DELAY);
    int status = request_len(method, path, body, NULL, NULL, &s_sync_conn);
    xSemaphoreGive(s_sync_lock);
    return status;
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static int book_cmp(const void *pa, const void *pb)
{
    const abs_book_t *a = pa, *b = pb;
    return text_cmp(a->sort_title, b->sort_title);
}

esp_err_t abs_parse_books(const char *items_json, const char *me_json, abs_book_t **out_books, int *out_count)
{
    *out_books = NULL;
    *out_count = 0;
    cJSON *root = cJSON_Parse(items_json);
    if (!root) return ESP_FAIL;
    const cJSON *results = cJSON_GetObjectItem(root, "results");
    int n = cJSON_GetArraySize(results);
    abs_book_t *books = heap_caps_calloc(n ? n : 1, sizeof(abs_book_t), MALLOC_CAP_SPIRAM);
    int count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, results) {
        const cJSON *media = cJSON_GetObjectItem(it, "media");
        const cJSON *meta = cJSON_GetObjectItem(media, "metadata");
        const char *id = json_str(it, "id");
        if (!id || !books) continue;
        abs_book_t *b = &books[count++];
        strlcpy(b->id, id, sizeof(b->id));
        b->title = psram_strdup(json_str(meta, "title") ?: "Untitled");
        b->sort_title = psram_strdup(json_str(meta, "titleIgnorePrefix") ?: b->title);
        b->added_at = json_num(it, "addedAt");
        b->podcast = strcmp(json_str(it, "mediaType") ?: "", "podcast") == 0;
        if (b->podcast) {
            b->author = psram_strdup(json_str(meta, "author") ?: "");
            b->num_episodes = (int)json_num(media, "numEpisodes");
        } else {
            b->author = psram_strdup(json_str(meta, "authorName") ?: "");
            b->duration = json_num(media, "duration");
        }
        b->series = psram_strdup(json_str(meta, "seriesName") ?: "");
    }
    cJSON_Delete(root);

    // Progress lives on the user record: per book, or per episode for podcasts.
    root = me_json ? cJSON_Parse(me_json) : NULL;
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(root, "mediaProgress")) {
        const char *item = json_str(p, "libraryItemId");
        const char *episode = json_str(p, "episodeId");
        if (!item) continue;
        for (int i = 0; i < count; i++) {
            abs_book_t *b = &books[i];
            if (strcmp(b->id, item) != 0) continue;
            const bool finished = cJSON_IsTrue(cJSON_GetObjectItem(p, "isFinished"));
            const double last = json_num(p, "lastUpdate");
            if (!b->podcast && !episode) {
                b->current_time = json_num(p, "currentTime");
                b->progress = json_num(p, "progress");
                b->finished = finished;
                b->last_update = last;
            } else if (b->podcast && episode && !finished && json_num(p, "currentTime") > 0 && last > b->last_update) {
                // A show is "in progress" through its most recently played unfinished episode.
                b->current_time = json_num(p, "currentTime");
                b->progress = json_num(p, "progress");
                b->last_update = last;
                strlcpy(b->resume_episode, episode, sizeof(b->resume_episode));
            }
            break;
        }
    }
    cJSON_Delete(root);

    qsort(books, count, sizeof(abs_book_t), book_cmp);
    *out_books = books;
    *out_count = count;
    return ESP_OK;
}

esp_err_t abs_parse_libraries(const char *json, abs_library_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    cJSON *root = cJSON_Parse(json);
    const cJSON *libs = cJSON_GetObjectItem(root, "libraries");
    int n = cJSON_GetArraySize(libs);
    abs_library_t *list = heap_caps_calloc(n ? n : 1, sizeof(abs_library_t), MALLOC_CAP_SPIRAM);
    const cJSON *lib;
    cJSON_ArrayForEach(lib, libs) {
        const char *id = json_str(lib, "id"), *type = json_str(lib, "mediaType");
        if (!id || !list) continue;
        abs_library_t *l = &list[(*count)++];
        strlcpy(l->id, id, sizeof(l->id));
        strlcpy(l->name, json_str(lib, "name") ?: "Library", sizeof(l->name));
        l->podcast = type && strcmp(type, "podcast") == 0;
    }
    cJSON_Delete(root);
    *out = list;
    return root ? ESP_OK : ESP_FAIL;
}

esp_err_t abs_get_libraries(abs_library_t **out, int *count, char **json)
{
    *out = NULL;
    *count = 0;
    *json = NULL;
    if (request(HTTP_METHOD_GET, "/api/libraries", NULL, json) != 200) {
        free(*json);
        *json = NULL;
        return ESP_FAIL;
    }
    return abs_parse_libraries(*json, out, count);
}

esp_err_t abs_get_books(const char *library_id, abs_book_t **out_books, int *out_count, char **items_json,
                        char **me_json)
{
    *out_books = NULL;
    *out_count = 0;
    *items_json = *me_json = NULL;
    char path[160];
    snprintf(path, sizeof(path), "/api/libraries/%s/items?limit=2000&minified=1", library_id);
    if (request(HTTP_METHOD_GET, path, NULL, items_json) != 200 ||
        request(HTTP_METHOD_GET, "/api/me", NULL, me_json) != 200) {
        free(*items_json);
        free(*me_json);
        *items_json = *me_json = NULL;
        return ESP_FAIL;
    }
    esp_err_t err = abs_parse_books(*items_json, *me_json, out_books, out_count);
    ESP_LOGI(TAG, "loaded %d items", *out_count);
    return err;
}

/* ---------- podcast episodes ---------- */

static int episode_cmp(const void *pa, const void *pb)
{
    const abs_episode_t *a = pa, *b = pb;
    bool ai = a->current_time > 0 && !a->finished, bi = b->current_time > 0 && !b->finished;
    if (ai != bi) return ai ? -1 : 1;
    if (ai && a->last_update != b->last_update) return a->last_update > b->last_update ? -1 : 1;
    return a->published_at > b->published_at ? -1 : (a->published_at < b->published_at ? 1 : 0);
}

esp_err_t abs_get_episodes(const char *item_id, abs_episode_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    char path[96], *body = NULL;
    snprintf(path, sizeof(path), "/api/items/%s?expanded=1", item_id);
    if (request(HTTP_METHOD_GET, path, NULL, &body) != 200) {
        free(body);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *episodes = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "media"), "episodes");
    int n = cJSON_GetArraySize(episodes);
    abs_episode_t *eps = heap_caps_calloc(n ? n : 1, sizeof(abs_episode_t), MALLOC_CAP_SPIRAM);
    const cJSON *e;
    cJSON_ArrayForEach(e, episodes) {
        const char *id = json_str(e, "id");
        if (!id || !eps) continue;
        abs_episode_t *ep = &eps[(*count)++];
        strlcpy(ep->id, id, sizeof(ep->id));
        ep->title = psram_strdup(json_str(e, "title") ?: "Episode");
        ep->duration = json_num(e, "duration");
        ep->published_at = json_num(e, "publishedAt");
    }
    cJSON_Delete(root);

    // Per-episode progress from the user record.
    if (request(HTTP_METHOD_GET, "/api/me", NULL, &body) == 200) {
        root = cJSON_Parse(body);
        const cJSON *p;
        cJSON_ArrayForEach(p, cJSON_GetObjectItem(root, "mediaProgress")) {
            const char *item = json_str(p, "libraryItemId"), *episode = json_str(p, "episodeId");
            if (!item || !episode || strcmp(item, item_id) != 0) continue;
            for (int i = 0; i < *count; i++) {
                if (strcmp(eps[i].id, episode) == 0) {
                    eps[i].current_time = json_num(p, "currentTime");
                    eps[i].progress = json_num(p, "progress");
                    eps[i].finished = cJSON_IsTrue(cJSON_GetObjectItem(p, "isFinished"));
                    eps[i].last_update = json_num(p, "lastUpdate");
                    break;
                }
            }
        }
        cJSON_Delete(root);
    }
    free(body);
    qsort(eps, *count, sizeof(abs_episode_t), episode_cmp);
    *out = eps;
    return ESP_OK;
}

void abs_free_episodes(abs_episode_t *eps, int count)
{
    for (int i = 0; i < count; i++) free(eps[i].title);
    free(eps);
}

char *abs_episodes_to_json(const abs_episode_t *eps, int count)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", eps[i].id);
        cJSON_AddStringToObject(o, "t", eps[i].title);
        cJSON_AddNumberToObject(o, "d", eps[i].duration);
        cJSON_AddNumberToObject(o, "p", eps[i].published_at);
        cJSON_AddNumberToObject(o, "c", eps[i].current_time);
        cJSON_AddNumberToObject(o, "g", eps[i].progress);
        cJSON_AddBoolToObject(o, "f", eps[i].finished);
        cJSON_AddNumberToObject(o, "u", eps[i].last_update);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

esp_err_t abs_episodes_from_json(const char *json, abs_episode_t **out, int *count)
{
    *out = NULL;
    *count = 0;
    cJSON *arr = cJSON_Parse(json);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return ESP_FAIL;
    }
    int n = cJSON_GetArraySize(arr);
    abs_episode_t *eps = heap_caps_calloc(n ? n : 1, sizeof(abs_episode_t), MALLOC_CAP_SPIRAM);
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        const char *id = json_str(o, "id");
        if (!id || !eps) continue;
        abs_episode_t *ep = &eps[(*count)++];
        strlcpy(ep->id, id, sizeof(ep->id));
        ep->title = psram_strdup(json_str(o, "t") ?: "Episode");
        ep->duration = json_num(o, "d");
        ep->published_at = json_num(o, "p");
        ep->current_time = json_num(o, "c");
        ep->progress = json_num(o, "g");
        ep->finished = cJSON_IsTrue(cJSON_GetObjectItem(o, "f"));
        ep->last_update = json_num(o, "u");
    }
    cJSON_Delete(arr);
    *out = eps;
    return ESP_OK;
}

const char *abs_library_name(void)
{
    return s_library_name;
}

void abs_set_library_name(const char *name)
{
    strlcpy(s_library_name, name ? name : "", sizeof(s_library_name));
}

const char *abs_server(void)
{
    const char *p = strstr(s_base, "://");
    return p ? p + 3 : s_base;
}

esp_err_t abs_patch_progress(const char *item_id, double current_time, double duration, bool finished)
{
    char path[96], body[160];
    snprintf(path, sizeof(path), "/api/me/progress/%s", item_id);
    snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"duration\":%.2f,\"progress\":%.5f,\"isFinished\":%s}",
             current_time, duration, duration > 0 ? current_time / duration : 0, finished ? "true" : "false");
    return sync_request(HTTP_METHOD_PATCH, path, body) == 200 ? ESP_OK : ESP_FAIL;
}

void abs_free_books(abs_book_t *books, int count)
{
    for (int i = 0; i < count; i++) {
        free(books[i].title);
        free(books[i].sort_title);
        free(books[i].author);
        free(books[i].series);
    }
    free(books);
}

esp_err_t abs_start_session(const char *item_id, const char *episode_id, bool for_download, abs_session_t *out)
{
    memset(out, 0, sizeof(*out));
    char path[140], req[320];
    if (episode_id && episode_id[0]) {
        snprintf(path, sizeof(path), "/api/items/%s/play/%s", item_id, episode_id);
    } else {
        snprintf(path, sizeof(path), "/api/items/%s/play", item_id);
    }
    // Only advertising audio/mpeg plus forceTranscode makes the server hand back an HLS stream of
    // MPEG-TS segments (AAC, or MP3 for MP3 sources) regardless of the book's file format.
    snprintf(req, sizeof(req),
             "{\"deviceInfo\":{\"clientName\":\"" DEVICE_NAME "\",\"deviceId\":\"%s\"},"
             "\"supportedMimeTypes\":[\"audio/mpeg\"],\"mediaPlayer\":\"esp32\",\"forceTranscode\":true}",
             // Downloads use their own device id so the server never treats them as the same
             // session as playback (it may close one device's session when another starts).
             for_download ? s_device_id_dl : s_device_id);
    char *body = NULL;
    if (request(HTTP_METHOD_POST, path, req, &body) != 200) {
        free(body);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const char *id = json_str(root, "id");
    const cJSON *track = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "audioTracks"), 0);
    const char *url = json_str(track, "contentUrl");
    if (!id || !url || strncmp(url, "/hls/", 5) != 0) {
        ESP_LOGE(TAG, "unexpected play response (playMethod %d)", (int)json_num(root, "playMethod"));
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strlcpy(out->id, id, sizeof(out->id));
    strlcpy(out->hls_path, url, sizeof(out->hls_path));
    out->current_time = json_num(root, "currentTime");
    out->duration = json_num(root, "duration");
    strlcpy(out->display_title, json_str(root, "displayTitle") ?: "", sizeof(out->display_title));
    strlcpy(out->display_author, json_str(root, "displayAuthor") ?: "", sizeof(out->display_author));

    const cJSON *chapters = cJSON_GetObjectItem(root, "chapters");
    int n = cJSON_GetArraySize(chapters);
    if (n > 0) {
        out->chapters = heap_caps_calloc(n, sizeof(abs_chapter_t), MALLOC_CAP_SPIRAM);
        const cJSON *ch;
        cJSON_ArrayForEach(ch, chapters) {
            if (!out->chapters) break;
            abs_chapter_t *c = &out->chapters[out->chapter_count++];
            c->title = psram_strdup(json_str(ch, "title") ?: "");
            c->start = json_num(ch, "start");
            c->end = json_num(ch, "end");
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "session %s at %.0f/%.0f s, %d chapters", out->id, out->current_time, out->duration,
             out->chapter_count);
    return ESP_OK;
}

void abs_free_session(abs_session_t *s)
{
    for (int i = 0; i < s->chapter_count; i++) {
        free(s->chapters[i].title);
    }
    free(s->chapters);
    memset(s, 0, sizeof(*s));
}

esp_err_t abs_sync_session(const char *session_id, double current_time, double time_listening, double duration)
{
    char path[96], body[160];
    snprintf(path, sizeof(path), "/api/session/%s/sync", session_id);
    snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"timeListening\":%.2f,\"duration\":%.2f}",
             current_time, time_listening, duration);
    return sync_request(HTTP_METHOD_POST, path, body) == 200 ? ESP_OK : ESP_FAIL;
}

esp_err_t abs_close_session(const char *session_id, double current_time, double time_listening)
{
    char path[96], body[128];
    snprintf(path, sizeof(path), "/api/session/%s/close", session_id);
    if (time_listening > 0) {
        snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"timeListening\":%.2f}", current_time, time_listening);
    } else {
        // Nothing was played; don't let the close create or move a progress record.
        strcpy(body, "{}");
    }
    return sync_request(HTTP_METHOD_POST, path, body) == 200 ? ESP_OK : ESP_FAIL;
}

abs_stream_t *abs_stream_create(void)
{
    return heap_caps_calloc(1, sizeof(abs_stream_t), MALLOC_CAP_SPIRAM);
}

int abs_stream_begin(abs_stream_t *st, const char *path)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", s_base, path);
    if (!st->client) {
        st->client = new_client(path, HTTP_METHOD_GET);
        if (!st->client) return -1;
    } else {
        esp_http_client_set_url(st->client, url);
    }
    const uint32_t gen = set_auth(st->client);
    esp_err_t err = esp_http_client_open(st->client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "segment connect failed: %s", esp_err_to_name(err));
        abs_stream_end(st, false);
        return -1;
    }
    if (esp_http_client_fetch_headers(st->client) < 0) {
        abs_stream_end(st, false);
        return -1;
    }
    int status = esp_http_client_get_status_code(st->client);
    if (status == 401 && try_refresh(gen)) {
        abs_stream_end(st, true);
        return abs_stream_begin(st, path);
    }
    return status;
}

/* ---------- signing in ---------- */

// A one-off request to any server (for the setup portal, before settings are saved, and for the
// token refresh itself). Returns the HTTP status, or -1 if the server couldn't be reached.
static int raw_request(const char *base, esp_http_client_method_t method, const char *path, const char *bearer,
                       const char *refresh, const char *body, char **out)
{
    *out = NULL;
    char url[256];
    snprintf(url, sizeof(url), "%s%s", base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = USER_AGENT,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    char *hdr = NULL;
    if (bearer && bearer[0]) {
        hdr = heap_caps_malloc(strlen(bearer) + 8, MALLOC_CAP_SPIRAM);
        sprintf(hdr, "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", hdr);
    }
    if (refresh) esp_http_client_set_header(c, "x-refresh-token", refresh);
    // Ask for the refresh token in the response body (otherwise it only goes in a cookie).
    esp_http_client_set_header(c, "x-return-tokens", "true");
    int len = body ? strlen(body) : 0;
    if (body) esp_http_client_set_header(c, "Content-Type", "application/json");
    int status = -1;
    if (esp_http_client_open(c, len) == ESP_OK && (!len || esp_http_client_write(c, body, len) == len) &&
        esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        size_t cap = 8192, n = 0;
        char *buf = psram_malloc(cap);
        int r;
        while (buf && (r = esp_http_client_read(c, buf + n, cap - n - 1)) > 0) {
            n += r;
            if (cap - n < 1024) {
                char *nb = heap_caps_realloc(buf, cap * 2, MALLOC_CAP_SPIRAM);
                if (!nb) break;
                buf = nb;
                cap *= 2;
            }
        }
        if (buf) buf[n] = 0;
        *out = buf;
    }
    esp_http_client_cleanup(c);
    free(hdr);
    return status;
}

// Tokens from a /login or /auth/refresh response (user.accessToken / user.refreshToken, or the
// pre-2.26 user.token).
// (yotwo) Copies the string value of "key" straight out of the text, without building the
// whole document. The reply to a sign-in or a renewal carries the user's entire listening
// history, and parsing that takes thousands of small allocations; were one to fail, the
// new tokens would be lost after the server had already retired the old ones.
static bool scan_token(const char *json, const char *key, char *out, size_t len)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) return false;
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (!end || end == p || (size_t)(end - p) >= len || memchr(p, '\\', end - p)) return false;
    memcpy(out, p, end - p);
    out[end - p] = 0;
    return true;
}

static bool parse_tokens(const char *json, char *access, size_t alen, char *refresh, size_t rlen)
{
    if (scan_token(json, "accessToken", access, alen)) {
        if (refresh && !scan_token(json, "refreshToken", refresh, rlen)) refresh[0] = 0;
        return true;
    }
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    const cJSON *user = cJSON_GetObjectItem(root, "user");
    const char *a = json_str(user, "accessToken");
    if (!a) a = json_str(root, "accessToken");
    if (!a) a = json_str(user, "token");
    const char *r = json_str(user, "refreshToken");
    if (!r) r = json_str(root, "refreshToken");
    if (a) strlcpy(access, a, alen);
    if (refresh) strlcpy(refresh, r ? r : "", rlen);
    cJSON_Delete(root);
    return a != NULL;
}

static bool try_refresh(uint32_t failed_gen)
{
    bool ok = false;
    xSemaphoreTake(s_refresh_lock, portMAX_DELAY);
    if (s_auth_gen != failed_gen) {
        ok = true;  // someone else already renewed it; just retry
    } else if (s_signed_out) {
        // (yotwo) Already refused: asking again cannot help, and counts against the server's limit on sign-in attempts.
    } else if (config_get()->refresh[0]) {
        char *body = NULL;
        char *access = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM), *refresh = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
        // (yotwo) The server retires the refresh token as it answers, and honours the old one
        // for only ten minutes more. A reply that is lost or unreadable is therefore asked for
        // again at once, rather than left to whichever request comes next.
        int status = 0;
        bool parsed = false;
        for (int attempt = 0; attempt < 4 && !parsed; attempt++) {
            if (attempt) {
                ESP_LOGW(TAG, "renewal attempt %d failed (%d, reply %d bytes); trying again", attempt, status,
                         body ? (int)strlen(body) : 0);
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            free(body);
            body = NULL;
            status = raw_request(s_base, HTTP_METHOD_POST, "/auth/refresh", NULL, config_get()->refresh, NULL, &body);
            if (status == 401 || status == 403) break;
            parsed = status == 200 && access && refresh && parse_tokens(body, access, 1024, refresh, 1024);
        }
        if (parsed) {
            ESP_LOGI(TAG, "renewal reply %d bytes", (int)strlen(body));
            // (yotwo) Which of these happened decides whether the next renewal can work.
            ESP_LOGI(TAG, "renewal: access token %d chars; refresh token %s", (int)strlen(access),
                     !refresh[0] ? "not returned (keeping the old one)"
                     : strcmp(refresh, config_get()->refresh) == 0 ? "returned unchanged" : "replaced by a new one");
            config_set_tokens(access, refresh[0] ? refresh : config_get()->refresh);
            xSemaphoreTake(s_auth_lock, portMAX_DELAY);
            snprintf(s_auth, 1100, "Bearer %s", access);
            s_auth_gen++;
            xSemaphoreGive(s_auth_lock);
            ESP_LOGI(TAG, "access token renewed");
            ok = true;
        } else {
            ESP_LOGW(TAG, "token refresh failed (%d); sign in again from Settings", status);
            ESP_LOGW(TAG, "(the refresh token sent was %d chars; reply: %.80s)", (int)strlen(config_get()->refresh),
                     body ? body : "none");
            if ((status == 401 || status == 403) && access && refresh) {
                // (yotwo) The session is over. With the password kept on the player, sign in
                // afresh; only if the server refuses that too is the player signed out.
                char pw[128];
                abs_auth_result_t r = ABS_AUTH_REJECTED;
                const bool kept = credential_load && config_get()->username[0] && credential_load(pw, sizeof(pw));
                if (kept) {
                    r = abs_login(s_base, config_get()->username, pw, access, 1024, refresh, 1024);
                    memset(pw, 0, sizeof(pw));
                }
                if (r == ABS_AUTH_OK) {
                    config_set_tokens(access, refresh);
                    xSemaphoreTake(s_auth_lock, portMAX_DELAY);
                    snprintf(s_auth, 1100, "Bearer %s", access);
                    s_auth_gen++;
                    xSemaphoreGive(s_auth_lock);
                    ESP_LOGI(TAG, "signed in again with the saved password");
                    ok = true;
                } else if (!kept || r == ABS_AUTH_REJECTED) {
                    if (kept) ESP_LOGW(TAG, "the saved password was refused too");
                    s_signed_out = true;
                } else {
                    ESP_LOGW(TAG, "could not reach the server to sign in again; will try later");
                }
            }
        }
        free(body);
        free(access);
        free(refresh);
    } else {
        s_signed_out = true;  // an API key that the server no longer accepts
    }
    xSemaphoreGive(s_refresh_lock);
    return ok;
}

bool abs_signed_out(void)
{
    return s_signed_out;
}

abs_auth_result_t abs_login(const char *base, const char *username, const char *password, char *access,
                            size_t alen, char *refresh, size_t rlen)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "username", username);
    cJSON_AddStringToObject(req, "password", password);
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    char *body = NULL;
    int status = raw_request(base, HTTP_METHOD_POST, "/login", NULL, NULL, json, &body);
    free(json);
    abs_auth_result_t r = status < 0              ? ABS_AUTH_UNREACHABLE
                          : status == 401          ? ABS_AUTH_REJECTED
                          : status != 200          ? ABS_AUTH_ERROR
                          : parse_tokens(body, access, alen, refresh, rlen) ? ABS_AUTH_OK
                                                                            : ABS_AUTH_ERROR;
    ESP_LOGI(TAG, "login to %s as %s: HTTP %d", base, username, status);
    free(body);
    return r;
}

abs_auth_result_t abs_check(const char *base, const char *token)
{
    char *body = NULL;
    int status = raw_request(base, HTTP_METHOD_GET, "/api/me", token, NULL, NULL, &body);
    free(body);
    return status < 0 ? ABS_AUTH_UNREACHABLE
           : status == 401 || status == 403 ? ABS_AUTH_REJECTED
           : status == 200 ? ABS_AUTH_OK
                           : ABS_AUTH_ERROR;
}

int abs_stream_read(abs_stream_t *st, char *buf, int len)
{
    return esp_http_client_read(st->client, buf, len);
}

bool abs_stream_complete(abs_stream_t *st)
{
    return st->client && esp_http_client_is_complete_data_received(st->client);
}

void abs_stream_end(abs_stream_t *st, bool keep_alive)
{
    if (!st->client) return;
    if (keep_alive) {
        // Drain anything left so the connection can carry the next request.
        esp_http_client_flush_response(st->client, NULL);
    } else {
        esp_http_client_cleanup(st->client);
        st->client = NULL;
    }
}

esp_err_t abs_get_cover(const char *item_id, int width, uint8_t **out, size_t *out_len)
{
    char path[128];
    // The server resizes and re-encodes, so this is a small baseline JPEG.
    snprintf(path, sizeof(path), "/api/items/%s/cover?width=%d&format=jpeg", item_id, width);
    char *body = NULL;
    // Only the cover loader task calls this, so it can own a persistent connection.
    static esp_http_client_handle_t conn;
    if (request_len(HTTP_METHOD_GET, path, NULL, &body, out_len, &conn) != 200 || *out_len == 0) {
        free(body);
        return ESP_FAIL;
    }
    *out = (uint8_t *)body;
    return ESP_OK;
}
