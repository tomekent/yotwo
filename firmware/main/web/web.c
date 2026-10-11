#include "web.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "battery.h"
#include "abs_api.h"
#include "account.h"
#include "cJSON.h"
#include "cards.h"
#include "cover16.h"
#include "download.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "names.h"
#include "nfc.h"
#include "offline.h"
#include "ota.h"
#include "player.h"
#include "player_settings.h"
#include "storage.h"
#include "ui.h"
#include "web_page.h"

static const char *TAG = "web";

static httpd_handle_t s_httpd;
static bool s_mdns;
static volatile bool s_answered;  // a data request has been answered since start-up

/* ---------- the name on the network ---------- */

void web_hostname(char *out, size_t len)
{
    // Letters and digits kept (in lower case), anything else between them becomes one dash;
    // apostrophes vanish, so "Tom's Yotwo" is toms-yotwo. Nothing usable: "yotwo".
    size_t n = 0;
    bool dash = false;
    for (const char *p = ui_player_name(); *p && n + 2 < len; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && n) out[n++] = '-';
            out[n++] = c;
            dash = false;
        } else if (c != '\'') {
            dash = true;
        }
    }
    out[n] = 0;
    if (!n) strlcpy(out, "yotwo", len);
}

static void announce(void)
{
    char host[48];
    web_hostname(host, sizeof(host));
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_set_hostname(sta, host);  // the name the router shows; used at the next lease
    if (!s_mdns) return;
    mdns_hostname_set(host);
    mdns_instance_name_set(ui_player_name());
    ESP_LOGI(TAG, "on the network as http://%s.local", host);
}

void web_init(void)
{
    if (mdns_init() == ESP_OK) {
        s_mdns = true;
        announce();
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS did not start: the page is reachable by IP address only");
    }
}

void web_name_changed(void)
{
    announce();
}

/* ---------- who may ask ---------- */

// The page has no password: it is meant to be opened from a phone on the home network with
// nothing to type. What it must not be is a way for some other web site, open in the same
// browser, to reach the player through that browser. So every request is checked:
//
//  - It must be addressed to the player by its own name or address (the Host header). A
//    site that points its own name at the player's address ("DNS rebinding") fails here.
//  - If the browser says where the request comes from (Origin, Sec-Fetch-Site), that must
//    be the player's own page.
//  - The data requests must carry the header the page's script adds (X-Yotwo). A form or a
//    link on another site cannot add it, and a script on another site that tries is first
//    made by the browser to ask permission (a "preflight"), which is never granted: no
//    cross-origin permission (CORS) header is ever sent.
//  - Requests that change anything must be JSON, which a plain form cannot send.
//
// Covers and files are fetched by <img> and links, which cannot add the header; for those
// the first two checks apply, and the reply says it may only be used by the player's page.

typedef struct {
    const char *host;        // Host header; NULL if absent
    const char *origin;      // Origin header; NULL if absent
    const char *fetch_site;  // Sec-Fetch-Site header; NULL if absent (older browsers, other tools)
    bool marked;             // the X-Yotwo header is present
    bool json;               // Content-Type is application/json
    bool page;               // the page itself: may be reached by a link from anywhere
    bool data;               // a data request (must be marked)
    bool change;             // changes something (must be JSON)
    bool upload;             // ...except this: a firmware file, sent as it is
} asker_t;

static bool same_host(const char *a, const char *b)
{
    return a && b && strcasecmp(a, b) == 0;
}

// `host` (which may end in ":80") is this player: its name with or without ".local", or its
// IP address.
static bool host_is_ours(const char *host, const char *name, const char *ip)
{
    if (!host || !host[0]) return false;
    char h[80], local[64];
    strlcpy(h, host, sizeof(h));
    char *colon = strrchr(h, ':');
    if (colon && strcmp(colon, ":80") == 0) *colon = 0;
    const size_t n = strlen(h);
    if (n && h[n - 1] == '.') h[n - 1] = 0;  // "yotwo.local." is the same name
    snprintf(local, sizeof(local), "%s.local", name);
    return same_host(h, local) || same_host(h, name) || (ip && ip[0] && same_host(h, ip));
}

// Why a request is refused, or NULL if it may go ahead.
static const char *refusal(const asker_t *a, const char *name, const char *ip)
{
    if (!host_is_ours(a->host, name, ip)) return "not addressed to this player";
    // The page alone may be opened by a link from elsewhere: it is the same for everyone,
    // holds nothing, and cannot be framed. Everything else must come from the page.
    if (!a->page && a->fetch_site && strcmp(a->fetch_site, "same-origin") != 0 && strcmp(a->fetch_site, "none") != 0) {
        return "from another site";
    }
    if (a->origin) {
        // Must be exactly the address the request itself was sent to.
        if (strncasecmp(a->origin, "http://", 7) != 0 || !same_host(a->origin + 7, a->host)) return "from another site";
    }
    if (a->change && a->fetch_site && strcmp(a->fetch_site, "same-origin") != 0) return "from another site";
    if ((a->data || a->change) && !a->marked) return "not from the player's page";
    if (a->change && !a->json && !a->upload) return "not JSON";
    return NULL;
}

static void my_ip(char *out, size_t len)
{
    out[0] = 0;
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) snprintf(out, len, IPSTR, IP2STR(&ip.ip));
}

// Headers for every reply: nothing may frame it, guess its type, or use it from another
// site; and nothing is cached.
static void harden(httpd_req_t *req, bool keep)
{
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Cross-Origin-Resource-Policy", "same-origin");
    // Covers may be kept by the browser for an hour (they do not change, and a list of them
    // would otherwise be read from the SD card again at every refresh); nothing else.
    httpd_resp_set_hdr(req, "Cache-Control", keep ? "private, max-age=3600" : "no-store");
}

typedef enum { ASK_PAGE, ASK_COVER, ASK_FILE, ASK_DATA, ASK_CHANGE, ASK_UPLOAD } ask_t;

// Checks the request and sets the reply's headers. False: it has been refused (403).
static bool admit(httpd_req_t *req, ask_t kind)
{
    char host[80] = "", origin[96] = "", site[24] = "", mark[8] = "", type[48] = "", name[48], ip[16];
    const bool has_host = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK;
    const bool has_origin = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK;
    const bool has_site = httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", site, sizeof(site)) == ESP_OK;
    httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type));
    const asker_t a = {
        .host = has_host ? host : NULL,
        .origin = has_origin ? origin : NULL,
        .fetch_site = has_site ? site : NULL,
        .marked = httpd_req_get_hdr_value_str(req, "X-Yotwo", mark, sizeof(mark)) == ESP_OK,
        .json = strncasecmp(type, "application/json", 16) == 0,
        .page = kind == ASK_PAGE,
        .data = kind == ASK_DATA || kind == ASK_CHANGE || kind == ASK_UPLOAD,
        .change = kind == ASK_CHANGE || kind == ASK_UPLOAD,
        .upload = kind == ASK_UPLOAD,
    };
    web_hostname(name, sizeof(name));
    my_ip(ip, sizeof(ip));
    harden(req, kind == ASK_COVER);
    const char *why = refusal(&a, name, ip);
    if (!why) {
        if (a.data) s_answered = true;
        return true;
    }
    ESP_LOGW(TAG, "refused %s: %s (host '%s', origin '%s', site '%s')", req->uri, why, host, origin, site);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Refused: this page only answers to itself.");
    return false;
}

// Asking permission on behalf of another site's script: never given.
static esp_err_t on_preflight(httpd_req_t *req)
{
    harden(req, false);
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- helpers ---------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = json ? httpd_resp_sendstr(req, json) : httpd_resp_send_500(req);
    free(json);
    return err;
}

static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return NULL;
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    int got = 0;
    while (body && got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) break;
        got += r;
    }
    cJSON *root = NULL;
    if (body && got == req->content_len) {
        body[got] = 0;
        root = cJSON_Parse(body);
    }
    if (body) memset(body, 0, req->content_len + 1);  // it may have carried a password
    free(body);
    return root;
}

// The "path" query parameter as a path on the SD card. Only absolute paths without "..",
// so nothing outside the card can be named.
static bool sd_path(httpd_req_t *req, char *out, size_t len)
{
    char query[300], value[256] = "/";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", value, sizeof(value));
    }
    // Undo %XX and '+' (file names have spaces).
    char *w = value;
    for (const char *r = value; *r; r++) {
        if (*r == '%' && r[1] && r[2]) {
            const char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r == '+' ? ' ' : *r;
        }
    }
    *w = 0;
    if (value[0] != '/' || strstr(value, "..")) return false;
    snprintf(out, len, "/sdcard%s", value);
    size_t n = strlen(out);
    while (n > 7 && out[n - 1] == '/') out[--n] = 0;  // no trailing slash (but keep "/sdcard")
    return true;
}

/* ---------- pages and data ---------- */

static esp_err_t on_page(httpd_req_t *req)
{
    if (!admit(req, ASK_PAGE)) return ESP_OK;
    httpd_resp_set_type(req, "text/html");
    // The page may run its own script and styles, show its own pictures and talk to itself;
    // nothing else, from anywhere.
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self'; "
                       "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    return httpd_resp_send(req, WEB_PAGE, sizeof(WEB_PAGE) - 1);
}

static const char *state_name(player_state_t s)
{
    switch (s) {
    case PLAYER_LOADING:
    case PLAYER_BUFFERING: return "loading";
    case PLAYER_PLAYING:   return "playing";
    case PLAYER_PAUSED:    return "paused";
    case PLAYER_FINISHED:  return "finished";
    case PLAYER_ERROR:     return "error";
    default:               return "idle";
    }
}

static esp_err_t on_state(httpd_req_t *req)
{
    if (!admit(req, ASK_DATA)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    char host[48], library[64];
    web_hostname(host, sizeof(host));
    ui_library_name(library, sizeof(library));
    cJSON_AddStringToObject(o, "name", ui_player_name());
    cJSON_AddStringToObject(o, "host", host);
    const esp_app_desc_t *app = esp_app_get_description();
    char build[64];
    snprintf(build, sizeof(build), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(o, "build", build);
    cJSON_AddStringToObject(o, "version", app->version);
    cJSON_AddStringToObject(o, "update", ota_state());
    char previous[32];
    if (ota_previous(previous, sizeof(previous))) cJSON_AddStringToObject(o, "previous", previous);
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "library", library);
    cJSON_AddBoolToObject(o, "signed_out", abs_signed_out());

    player_status_t st;
    player_get_status(&st);
    cJSON *p = cJSON_AddObjectToObject(o, "playing");
    cJSON_AddStringToObject(p, "state", state_name(st.state));
    if (st.state != PLAYER_IDLE) {
        cJSON_AddStringToObject(p, "id", st.item_id);
        cJSON_AddStringToObject(p, "title", st.title);
        cJSON_AddStringToObject(p, "author", st.author);
        cJSON_AddStringToObject(p, "chapter", st.chapter);
        cJSON_AddNumberToObject(p, "chapter_index", st.chapter_index);
        cJSON_AddNumberToObject(p, "chapter_count", st.chapter_count);
        cJSON_AddNumberToObject(p, "position", st.position);
        cJSON_AddNumberToObject(p, "duration", st.duration);
        cJSON_AddBoolToObject(p, "from_card", download_state(st.item_id, NULL) == DL_DONE && !st.episode_id[0]);
    }
    cJSON_AddNumberToObject(o, "volume", ui_knob_volume());

    battery_status_t b;
    battery_get(&b);
    cJSON *bat = cJSON_AddObjectToObject(o, "battery");
    cJSON_AddNumberToObject(bat, "percent", b.percent);
    cJSON_AddNumberToObject(bat, "volts", (double)((int)(b.volts * 100 + 0.5f)) / 100);
    cJSON_AddBoolToObject(bat, "plugged", b.charging || b.charged);
    cJSON_AddBoolToObject(bat, "charging", b.charging);

    wifi_ap_record_t ap;
    cJSON *net = cJSON_AddObjectToObject(o, "wifi");
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(net, "ssid", (const char *)ap.ssid);
        cJSON_AddNumberToObject(net, "rssi", ap.rssi);
    }
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        char text[16];
        snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(net, "ip", text);
    }

    const time_t now = time(NULL);
    if (now > 1600000000) {
        struct tm tm;
        char text[24];
        localtime_r(&now, &tm);
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
        cJSON_AddStringToObject(o, "time", text);
    }
    uint64_t free_b = 0, total_b = 0;
    storage_space(&free_b, &total_b);
    cJSON *sd = cJSON_AddObjectToObject(o, "sd");
    cJSON_AddBoolToObject(sd, "present", storage_ready());
    cJSON_AddNumberToObject(sd, "free_mb", (double)(free_b >> 20));
    cJSON_AddNumberToObject(sd, "total_mb", (double)(total_b >> 20));
    cJSON *mem = cJSON_AddObjectToObject(o, "memory");
    cJSON_AddNumberToObject(mem, "internal_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    cJSON_AddNumberToObject(mem, "psram_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return send_json(req, o);
}

static esp_err_t send_settings(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    player_settings_to_json(o);
    return send_json(req, o);
}

static esp_err_t on_settings_get(httpd_req_t *req)
{
    return admit(req, ASK_DATA) ? send_settings(req) : ESP_OK;
}

static esp_err_t on_settings_post(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    cJSON *root = read_json(req);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    player_settings_from_json(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "settings changed from the web page");
    return send_settings(req);
}

static const char *dl_state_name(dl_state_t s)
{
    switch (s) {
    case DL_QUEUED:   return "waiting";
    case DL_ACTIVE:   return "downloading";
    case DL_DONE:     return "complete";
    case DL_ERROR:    return "failed";
    case DL_REMOVING: return "removing";
    default:          return "none";
    }
}

// Every book that is kept, wanted or was told not to be kept, with what is on the card.
static esp_err_t send_downloads(httpd_req_t *req)
{
    enum { MAX = 200 };
    char (*wanted)[40] = heap_caps_malloc(MAX * 40, MALLOC_CAP_SPIRAM);
    dl_info_t *have = heap_caps_malloc(64 * sizeof(dl_info_t), MALLOC_CAP_SPIRAM);
    if (!wanted || !have) {
        free(wanted);
        free(have);
        return httpd_resp_send_500(req);
    }
    const int n_wanted = offline_wanted(wanted, MAX);
    const int n_have = download_list(have, 64);
    uint8_t *cover = heap_caps_malloc(COVER16_BYTES, MALLOC_CAP_SPIRAM);
    cJSON *arr = cJSON_CreateArray();
    // First what is on the card (or on its way), then what is wanted and has nothing yet.
    for (int pass = 0; pass < 2; pass++) {
        const int n = pass == 0 ? n_have : n_wanted;
        for (int i = 0; i < n; i++) {
            const char *id = pass == 0 ? have[i].id : wanted[i];
            bool listed = false, is_wanted = false;
            for (int k = 0; k < n_have && pass == 1 && !listed; k++) listed = strcmp(have[k].id, id) == 0;
            if (listed) continue;
            for (int k = 0; k < n_wanted && !is_wanted; k++) is_wanted = strcmp(wanted[k], id) == 0;
            char title[128] = "", author[96] = "", path[96];
            // The loaded library, a card, or else (another library's favourite) the names
            // looked up on the server, which also fetches its cover for next time.
            if (!ui_book_names(id, title, sizeof(title), author, sizeof(author)) &&
                !cards_names(id, title, sizeof(title), author, sizeof(author))) {
                names_get(id, title, sizeof(title), author, sizeof(author));
                if (cover) cover16_get(id, cover);
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", id);
            cJSON_AddStringToObject(o, "title", title);
            cJSON_AddStringToObject(o, "author", author);
            cJSON_AddStringToObject(o, "state", pass == 0 ? dl_state_name(have[i].state) : "none");
            cJSON_AddNumberToObject(o, "percent", pass == 0 ? have[i].percent : 0);
            cJSON_AddBoolToObject(o, "wanted", is_wanted);
            cJSON_AddBoolToObject(o, "skipped", offline_skipped(id));
            snprintf(path, sizeof(path), STORAGE_ROOT "/dl/%s/audio.ts", id);
            const long bytes = pass == 0 ? storage_file_size(path) : -1;
            cJSON_AddNumberToObject(o, "mb", bytes > 0 ? (double)(bytes >> 20) : 0);
            cJSON_AddItemToArray(arr, o);
        }
    }
    free(wanted);
    free(have);
    free(cover);
    return send_json(req, arr);
}

// {"id": "...", "keep": false} removes a copy and stops it being fetched again;
// {"id": "...", "keep": true} lets it be fetched again.
static esp_err_t on_downloads(httpd_req_t *req)
{
    return admit(req, ASK_DATA) ? send_downloads(req) : ESP_OK;
}

static esp_err_t on_download_post(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    cJSON *root = read_json(req);
    const cJSON *id = cJSON_GetObjectItem(root, "id"), *keep = cJSON_GetObjectItem(root, "keep");
    if (!cJSON_IsString(id) || !cJSON_IsBool(keep) || strlen(id->valuestring) >= 40 || strchr(id->valuestring, '/')) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    }
    if (cJSON_IsTrue(keep)) {
        offline_skip(id->valuestring, false);
    } else {
        // A wanted book is put on the skip list (which removes its copy); one that is no
        // longer wanted just has its copy removed.
        enum { MAX = 200 };
        char (*wanted)[40] = heap_caps_malloc(MAX * 40, MALLOC_CAP_SPIRAM);
        const int n = wanted ? offline_wanted(wanted, MAX) : 0;
        bool is_wanted = false;
        for (int i = 0; i < n && !is_wanted; i++) is_wanted = strcmp(wanted[i], id->valuestring) == 0;
        free(wanted);
        if (is_wanted) offline_skip(id->valuestring, true);
        else download_remove(id->valuestring);
        ESP_LOGI(TAG, "copy of %.8s removed from the web page", id->valuestring);
    }
    cJSON_Delete(root);
    return send_downloads(req);
}

// Every card the player knows: what it plays (or that it is a sleep card), and whether it
// is the one in the slot now.
static esp_err_t send_cards(httpd_req_t *req)
{
    card_t *cards = heap_caps_malloc(64 * sizeof(card_t), MALLOC_CAP_SPIRAM);
    if (!cards) return httpd_resp_send_500(req);
    const int n = cards_list(cards, 64);
    char slot[16];
    nfc_uid_text(slot, sizeof(slot));
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "uid", cards[i].uid);
        cJSON_AddStringToObject(o, "kind", cards[i].kind == CARD_SLEEP ? "sleep" : "book");
        if (cards[i].kind == CARD_SLEEP) {
            cJSON_AddNumberToObject(o, "minutes", cards[i].minutes);
        } else {
            cJSON_AddStringToObject(o, "item", cards[i].item);
            cJSON_AddStringToObject(o, "title", cards[i].title);
            cJSON_AddStringToObject(o, "author", cards[i].author);
            cJSON_AddBoolToObject(o, "podcast", cards[i].podcast);
        }
        cJSON_AddBoolToObject(o, "in_slot", slot[0] && strcmp(slot, cards[i].uid) == 0);
        cJSON_AddItemToArray(arr, o);
    }
    free(cards);
    return send_json(req, arr);
}

static esp_err_t on_cards(httpd_req_t *req)
{
    return admit(req, ASK_DATA) ? send_cards(req) : ESP_OK;
}

// {"uid": "...", "unlink": true} forgets a card: it becomes a new card again.
static esp_err_t on_card_post(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    cJSON *root = read_json(req);
    const cJSON *uid = cJSON_GetObjectItem(root, "uid");
    card_t found;
    if (!cJSON_IsString(uid) || !cJSON_IsTrue(cJSON_GetObjectItem(root, "unlink")) ||
        !cards_find(uid->valuestring, &found)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    }
    cards_remove(found.uid);
    ESP_LOGI(TAG, "card %s unlinked from the web page", found.uid);
    cJSON_Delete(root);
    return send_cards(req);
}

// A folder of the SD card: names, sizes, which are folders. For finding faults.
// (The reading is done by list_job, on the storage helper's stack: see storage.h.)
typedef struct { const char *path; cJSON *arr; bool found; } list_job_t;

static void list_job(void *arg)
{
    list_job_t *j = arg;
    DIR *d = opendir(j->path);
    if (!d) return;
    j->found = true;
    struct dirent *e;
    int count = 0;
    char *full = heap_caps_malloc(560, MALLOC_CAP_SPIRAM);
    while (full && (e = readdir(d)) && count < 500) {
        struct stat sb;
        snprintf(full, 560, "%s/%s", j->path, e->d_name);
        const bool ok = stat(full, &sb) == 0;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->d_name);
        cJSON_AddBoolToObject(o, "dir", e->d_type == DT_DIR);
        cJSON_AddNumberToObject(o, "size", ok && e->d_type != DT_DIR ? (double)sb.st_size : 0);
        cJSON_AddItemToArray(j->arr, o);
        count++;
    }
    free(full);
    closedir(d);
}

static esp_err_t on_files(httpd_req_t *req)
{
    if (!admit(req, ASK_DATA)) return ESP_OK;
    char path[280];
    if (!storage_ready() || !sd_path(req, path, sizeof(path))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    list_job_t j = { path, cJSON_CreateArray(), false };
    storage_call(list_job, &j);
    if (!j.found) {
        cJSON_Delete(j.arr);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    }
    return send_json(req, j.arr);
}

// One file from the SD card, as it is. Opened, read a block at a time and closed by
// file_job on the storage helper's stack; sent from here.
typedef struct { const char *path; FILE *f; char *buf; size_t n; enum { F_OPEN, F_READ, F_CLOSE } what; } file_job_t;

static void file_job(void *arg)
{
    file_job_t *j = arg;
    if (j->what == F_OPEN) j->f = fopen(j->path, "rb");
    else if (j->what == F_READ) j->n = fread(j->buf, 1, 2048, j->f);
    else fclose(j->f);
}

static esp_err_t on_file(httpd_req_t *req)
{
    if (!admit(req, ASK_FILE)) return ESP_OK;
    char path[280];
    if (!storage_ready() || !sd_path(req, path, sizeof(path))) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    file_job_t j = { .path = path, .what = F_OPEN };
    storage_call(file_job, &j);
    if (!j.f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    const char *dot = strrchr(path, '.');
    const bool text = dot && (!strcmp(dot, ".json") || !strcmp(dot, ".txt") || !strcmp(dot, ".log"));
    httpd_resp_set_type(req, text ? "text/plain; charset=utf-8" : "application/octet-stream");
    // Whatever the file holds, the browser is to show or save it, never run it.
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; sandbox");
    // Read through internal memory: SD transfers straight to or from PSRAM are not reliable
    // on this board (see storage.c).
    j.buf = heap_caps_malloc(2048, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    esp_err_t err = j.buf ? ESP_OK : ESP_FAIL;
    j.what = F_READ;
    while (err == ESP_OK) {
        storage_call(file_job, &j);
        if (j.n == 0) break;
        err = httpd_resp_send_chunk(req, j.buf, j.n);
    }
    free(j.buf);
    j.what = F_CLOSE;
    storage_call(file_job, &j);
    if (err != ESP_OK) return ESP_FAIL;  // closes the connection
    return httpd_resp_send_chunk(req, NULL, 0);
}

// A book's cover: the small JPEG kept from the server if there is one, else the 16 x 16
// picture the player itself shows, as a BMP.
static esp_err_t on_cover(httpd_req_t *req)
{
    if (!admit(req, ASK_COVER)) return ESP_OK;
    char query[80], id[40] = "", path[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) httpd_query_key_value(query, "id", id, sizeof(id));
    bool plain = id[0] != 0;
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) plain = false;
    }
    if (!plain || !storage_ready()) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    size_t len = 0;
    snprintf(path, sizeof(path), COVER16_JPEG_PATH, id);
    char *file = storage_read_file(path, &len);
    if (file && len > 100) {
        // Nearly always a JPEG; say what it really is, since the browser is told not to guess.
        const bool png = len > 8 && memcmp(file, "\x89PNG", 4) == 0;
        const bool webp = len > 12 && memcmp(file, "RIFF", 4) == 0 && memcmp(file + 8, "WEBP", 4) == 0;
        httpd_resp_set_type(req, png ? "image/png" : webp ? "image/webp" : "image/jpeg");
        const esp_err_t err = httpd_resp_send(req, file, len);
        free(file);
        return err;
    }
    free(file);
    snprintf(path, sizeof(path), COVER16_RGB_PATH, id);
    file = storage_read_file(path, &len);
    if (!file || len != COVER16_BYTES) {
        free(file);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    }
    // A 24-bit BMP: 54 bytes of header, then rows from the bottom up, blue first.
    uint8_t bmp[54 + COVER16_BYTES] = { 'B', 'M', (uint8_t)sizeof(bmp), (uint8_t)(sizeof(bmp) >> 8), 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
                                        40, 0, 0, 0, 16, 0, 0, 0, 16, 0, 0, 0, 1, 0, 24, 0 };
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            const uint8_t *px = (const uint8_t *)file + ((15 - y) * 16 + x) * 3;
            uint8_t *o = bmp + 54 + (y * 16 + x) * 3;
            o[0] = px[2]; o[1] = px[1]; o[2] = px[0];
        }
    }
    free(file);
    httpd_resp_set_type(req, "image/bmp");
    return httpd_resp_send(req, (const char *)bmp, sizeof(bmp));
}

/* ---------- firmware update ---------- */

// Asks the player to show a code. The upload that follows must carry it.
static esp_err_t on_update_code(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    ota_new_code();
    return send_json(req, cJSON_CreateObject());
}

static void restart_now(void *arg)
{
    esp_restart();
}

static esp_err_t update_reply(httpd_req_t *req, bool ok, const char *error)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    if (error) cJSON_AddStringToObject(o, "error", error);
    if (!ok) {
        ESP_LOGW(TAG, "update refused: %s", error ? error : "");
        ui_show_word("FAIL", 5);
    }
    return send_json(req, o);
}

// The firmware file itself, as the body of the request, with ?code= from the display.
static esp_err_t on_update(httpd_req_t *req)
{
    if (!admit(req, ASK_UPLOAD)) return ESP_OK;
    char query[32], code[8] = "", why[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) httpd_query_key_value(query, "code", code, sizeof(code));
    // The body must still be read (or the connection is left in a muddle), so refusals
    // come after it for anything but a wrong code, which is cheap to refuse at once.
    if (!ota_code_ok(code)) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return update_reply(req, false, "That is not the code on the player (or it has run out). Ask for a new one.");
    }
    const size_t total = req->content_len;
    if (total < 64 * 1024 || total > 4 * 1024 * 1024) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return update_reply(req, false, "That does not look like a firmware file (wrong size).");
    }
    ESP_LOGI(TAG, "firmware upload of %u bytes begins", (unsigned)total);
    player_stop();
    ui_show_word("UPD", 600);
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = buf ? ota_begin(total) : ESP_ERR_NO_MEM;
    if (err == ESP_ERR_NOT_FOUND) {
        free(buf);
        httpd_resp_set_hdr(req, "Connection", "close");
        return update_reply(req, false, "This player's flash has no second firmware slot yet: one last update by cable is needed first.");
    }
    size_t got = 0;
    int stalls = 0;
    while (err == ESP_OK && got < total) {
        const int n = httpd_req_recv(req, buf, total - got < 4096 ? total - got : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 6) continue;  // a slow phone: wait up to half a minute
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        stalls = 0;
        err = ota_write(buf, (size_t)n);
        got += (size_t)n;
    }
    free(buf);
    if (err != ESP_OK) {
        ota_abort();
        httpd_resp_set_hdr(req, "Connection", "close");
        return update_reply(req, false, got < total ? "The upload stopped part-way. Nothing was changed." : "Could not write the firmware. Nothing was changed.");
    }
    if (ota_end(why, sizeof(why)) != ESP_OK) return update_reply(req, false, why);
    ui_show_word("DONE", 5);
    const esp_err_t sent = update_reply(req, true, NULL);
    // Restart once the reply has had time to go.
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = restart_now, .name = "restart" };
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_once(timer, 2000000);
    return sent;
}

// {"code": "1234"}: back to the firmware in the other slot (on trial), then restart.
static esp_err_t on_update_back(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    cJSON *root = read_json(req);
    const cJSON *code = cJSON_GetObjectItem(root, "code");
    const bool ok_code = cJSON_IsString(code) && ota_code_ok(code->valuestring);
    cJSON_Delete(root);
    if (!ok_code) return update_reply(req, false, "That is not the code on the player (or it has run out). Ask for a new one.");
    char why[96];
    if (ota_go_back(why, sizeof(why)) != ESP_OK) return update_reply(req, false, why);
    player_stop();
    ui_show_word("BACK", 5);
    const esp_err_t sent = update_reply(req, true, NULL);
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = restart_now, .name = "restart" };
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_once(timer, 2000000);
    return sent;
}

/* ---------- the server and sign-in ---------- */

static esp_err_t on_account(httpd_req_t *req)
{
    if (!admit(req, ASK_DATA)) return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    account_to_json(o);
    return send_json(req, o);
}

// Asks the player to show a code (the same kind as for a firmware update).
static esp_err_t on_account_code(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    ota_new_code();
    return send_json(req, cJSON_CreateObject());
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

// {"code", "server", "auth", "username", "password", "api_key", "remember"}: checked with the
// server (seconds), saved, and the player restarts to load the library from it.
static esp_err_t on_account_post(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    cJSON *root = read_json(req);
    cJSON *o = cJSON_CreateObject();
    const cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsString(code) || !ota_code_ok(code->valuestring)) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "That is not the code on the player (or it has run out). Ask for a new one.");
    } else {
        const account_request_t a = {
            .server = jstr(root, "server"),
            .auth = jstr(root, "auth"),
            .username = jstr(root, "username"),
            .password = jstr(root, "password"),
            .api_key = jstr(root, "api_key"),
            .remember = !cJSON_IsFalse(cJSON_GetObjectItem(root, "remember")),
        };
        char msg[200];
        ui_show_word("SIGN", 30);
        const bool ok = account_apply(&a, msg, sizeof(msg));
        ui_show_word(ok ? "DONE" : "FAIL", 5);
        cJSON_AddBoolToObject(o, "ok", ok);
        cJSON_AddStringToObject(o, ok ? "message" : "error", msg);
        if (ok) {
            static esp_timer_handle_t timer;
            const esp_timer_create_args_t args = { .callback = restart_now, .name = "restart" };
            if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_once(timer, 2000000);
        }
    }
    cJSON *pw = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(pw)) memset(pw->valuestring, 0, strlen(pw->valuestring));
    cJSON_Delete(root);
    return send_json(req, o);
}

/* ---------- display test ---------- */

static esp_err_t on_display_test(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    ESP_LOGI(TAG, "display test from the web page");
    ui_display_test();
    return send_json(req, cJSON_CreateObject());
}

/* ---------- formatting the SD card ---------- */

// Asks the player to show a code (the same kind as for a firmware update).
static esp_err_t on_format_code(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    ota_new_code();
    return send_json(req, cJSON_CreateObject());
}

// {"code": "1234"}: wipes the card, puts the linked cards back on it, and restarts.
static esp_err_t on_format(httpd_req_t *req)
{
    if (!admit(req, ASK_CHANGE)) return ESP_OK;
    cJSON *root = read_json(req);
    const cJSON *code = cJSON_GetObjectItem(root, "code");
    const bool ok_code = cJSON_IsString(code) && ota_code_ok(code->valuestring);
    cJSON_Delete(root);
    cJSON *o = cJSON_CreateObject();
    if (!ok_code) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "That is not the code on the player (or it has run out). Ask for a new one.");
        return send_json(req, o);
    }
    ESP_LOGW(TAG, "SD card format asked for from the web page");
    player_stop();
    download_set_paused(true);
    ui_show_word("FMT", 600);
    vTaskDelay(pdMS_TO_TICKS(3000));  // the downloader lets go of its files within a segment
    const esp_err_t err = storage_format();
    if (err == ESP_OK) cards_resave();  // the linked cards are in memory: keep them
    ui_show_word(err == ESP_OK ? "DONE" : "FAIL", 5);
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(o, "error", err == ESP_ERR_INVALID_STATE ? "The card slot is not working."
                                            : "The card could not be formatted. Is there a card in the player?");
    }
    const esp_err_t sent = send_json(req, o);
    // Restart either way: what the player had open or remembered about the card is stale.
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = restart_now, .name = "restart" };
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_once(timer, 2000000);
    return sent;
}

/* ---------- the server ---------- */

void web_start(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // internal RAM is scarce
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 22;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 3;  // sockets are shared with playback, downloads and the rest
    cfg.lru_purge_enable = true;
    cfg.core_id = 1;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        s_httpd = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = on_page },
        { .uri = "/api/state", .method = HTTP_GET, .handler = on_state },
        { .uri = "/api/settings", .method = HTTP_GET, .handler = on_settings_get },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = on_settings_post },
        { .uri = "/api/downloads", .method = HTTP_GET, .handler = on_downloads },
        { .uri = "/api/downloads", .method = HTTP_POST, .handler = on_download_post },
        { .uri = "/api/cards", .method = HTTP_GET, .handler = on_cards },
        { .uri = "/api/cards", .method = HTTP_POST, .handler = on_card_post },
        { .uri = "/api/files", .method = HTTP_GET, .handler = on_files },
        { .uri = "/api/file", .method = HTTP_GET, .handler = on_file },
        { .uri = "/api/cover", .method = HTTP_GET, .handler = on_cover },
        { .uri = "/api/update/code", .method = HTTP_POST, .handler = on_update_code },
        { .uri = "/api/update", .method = HTTP_POST, .handler = on_update },
        { .uri = "/api/update/back", .method = HTTP_POST, .handler = on_update_back },
        { .uri = "/api/account", .method = HTTP_GET, .handler = on_account },
        { .uri = "/api/account/code", .method = HTTP_POST, .handler = on_account_code },
        { .uri = "/api/account", .method = HTTP_POST, .handler = on_account_post },
        { .uri = "/api/display/test", .method = HTTP_POST, .handler = on_display_test },
        { .uri = "/api/format/code", .method = HTTP_POST, .handler = on_format_code },
        { .uri = "/api/format", .method = HTTP_POST, .handler = on_format },
        { .uri = "/*", .method = HTTP_OPTIONS, .handler = on_preflight },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    char host[48];
    web_hostname(host, sizeof(host));
    ESP_LOGI(TAG, "web page up at http://%s.local (internal RAM free %u)", host,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool web_running(void)
{
    return s_httpd != NULL;
}

// The player asks its own page for its state, as the page in a browser would: through the
// network stack, the server's task and the same checks. Throws the answer away.
static void ask_myself(void)
{
    char host[64], name[48];
    web_hostname(name, sizeof(name));
    snprintf(host, sizeof(host), "%s.local", name);
    const esp_http_client_config_t cfg = { .url = "http://127.0.0.1/api/state", .timeout_ms = 5000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    esp_http_client_set_header(c, "Host", host);
    esp_http_client_set_header(c, "X-Yotwo", "1");
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200) ESP_LOGW(TAG, "asking my own page: %s, status %d", esp_err_to_name(err), status);
}

bool web_check(void)
{
    static int64_t last;
    if (s_answered || !s_httpd) return s_answered;
    const int64_t now = esp_timer_get_time();
    if (last && now - last < 15 * 1000000LL) return false;
    last = now;
    ask_myself();
    return s_answered;
}

void web_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    ESP_LOGI(TAG, "web page stopped");
}
