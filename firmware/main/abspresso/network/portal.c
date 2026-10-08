#include "portal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "abs_api.h"
#include "cJSON.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "portal_page.h"
#include "power.h"
#include "wifi.h"

static const char *TAG = "portal";

#define AP_IP "192.168.4.1"

static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task, s_apply_task;
static volatile bool s_active, s_dns_stop;
static char s_ssid[33], s_pass[16];

// Progress of the last save, shown on the page and the device screen.
static SemaphoreHandle_t s_lock;
static char s_message[160] = "Waiting for your phone or laptop";
static bool s_busy;
static cJSON *s_pending;  // the submitted settings, handed to the apply task
static volatile bool s_bring_up;  // the setup task should scan, then start the access point

// Nearby networks, scanned once before the setup network starts: scanning takes the radio off
// the setup network's channel for seconds, which drops the phone that asked for it.
#define MAX_SCAN 20
static wifi_ap_record_t *s_scan;
static volatile int s_scan_count = -1;  // -1 until the scan finishes

static void set_status(bool busy, const char *fmt, ...)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_message, sizeof(s_message), fmt, ap);
    va_end(ap);
    s_busy = busy;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", s_message);
}

/* ---------- DNS: every name resolves to us, so phones open the setup page ---------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (!s_dns_stop) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        // Turn the query into a response with one A record pointing at the setup address.
        buf[2] = 0x81;  // response, recursion desired
        buf[3] = 0x80;  // recursion available, no error
        buf[6] = 0;     // one answer
        buf[7] = 1;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(buf + n, answer, sizeof(answer));
        sendto(sock, buf, n + sizeof(answer), 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

/* ---------- HTTP ---------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t on_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_PAGE, sizeof(PORTAL_PAGE) - 1);
}

// Anything else (including the OS "is there internet?" probes) goes to the setup page.
static esp_err_t on_other(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t on_config(httpd_req_t *req)
{
    const app_config_t *c = config_get();
    power_config_t pc;
    power_get_config(&pc);
    // Never send stored passwords or tokens back to the browser.
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", c->wifi_ssid);
    cJSON_AddStringToObject(o, "server", c->server);
    cJSON_AddStringToObject(o, "username", c->username);
    cJSON_AddStringToObject(o, "auth", (c->access[0] && !c->refresh[0] && !c->username[0]) ? "apikey" : "password");
    cJSON_AddBoolToObject(o, "signed_in", c->access[0] != 0);
    cJSON_AddNumberToObject(o, "brightness", pc.brightness);
    cJSON_AddNumberToObject(o, "screen_off", pc.screen_off_s);
    cJSON_AddNumberToObject(o, "sleep", pc.sleep_min);
    cJSON_AddNumberToObject(o, "skip_back", c->skip_back_s);
    cJSON_AddNumberToObject(o, "skip_fwd", c->skip_fwd_s);
    cJSON_AddBoolToObject(o, "rotate", c->rotate180);
    return send_json(req, o);
}

static esp_err_t on_scan(httpd_req_t *req)
{
    const wifi_ap_record_t *aps = s_scan;
    const int n = s_scan_count;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, arr);
}

static esp_err_t on_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(o, "message", s_message);
    cJSON_AddBoolToObject(o, "busy", s_busy);
    xSemaphoreGive(s_lock);
    return send_json(req, o);
}

static esp_err_t on_save(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    int got = 0;
    while (body && got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) break;
        got += r;
    }
    cJSON *root = NULL;
    if (body && got == req->content_len) {
        body[got] = 0;
        root = cJSON_Parse(body);
    }
    free(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_busy;
    if (!busy) {
        cJSON_Delete(s_pending);
        s_pending = root;
        s_busy = true;
        strlcpy(s_message, "Saving...", sizeof(s_message));
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        cJSON_Delete(root);
    } else {
        xTaskNotifyGive(s_apply_task);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "accepted", !busy);
    return send_json(req, o);
}

/* ---------- applying a save ---------- */

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int num(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static void apply(const cJSON *req)
{
    app_config_t *c = heap_caps_malloc(sizeof(*c), MALLOC_CAP_SPIRAM);
    *c = *config_get();

    // Wi-Fi: blank password means keep the saved one (if the network didn't change).
    const char *ssid = str(req, "ssid"), *wpass = str(req, "wifi_password");
    if (!ssid[0]) {
        set_status(false, "Choose a Wi-Fi network.");
        goto out;
    }
    const bool same_net = strcmp(ssid, c->wifi_ssid) == 0;
    const char *use_pass = (wpass[0] || !same_net) ? wpass : c->wifi_pass;
    // Already on that network with those details: no need to test (and hop channels).
    const bool unchanged = same_net && strcmp(use_pass, c->wifi_pass) == 0 && wifi_is_connected();
    if (!unchanged) set_status(true, "Connecting to %s...", ssid);
    if (!unchanged && !wifi_try_connect(ssid, use_pass, 20000)) {
        set_status(false, "Couldn't join %s. Check the network and password.", ssid);
        goto out;
    }
    strlcpy(c->wifi_ssid, ssid, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, use_pass, sizeof(c->wifi_pass));

    // Server: trimmed, no trailing slash, https:// assumed if no scheme given.
    char server[128];
    const char *srv = str(req, "server");
    if (!srv[0]) {
        set_status(false, "Enter your Audiobookshelf server address.");
        goto out;
    }
    snprintf(server, sizeof(server), "%s%s", strstr(srv, "://") ? "" : "https://", srv);
    size_t n = strlen(server);
    while (n && server[n - 1] == '/') server[--n] = 0;

    // Sign in: username/password, an API key, or keep the current credential.
    abs_auth_result_t r;
    const char *mode = str(req, "auth"), *user = str(req, "username"), *pass = str(req, "password");
    const char *key = str(req, "api_key");
    if (strcmp(mode, "apikey") == 0 && key[0]) {
        set_status(true, "Checking the API key...");
        r = abs_check(server, key);
        if (r == ABS_AUTH_OK) {
            strlcpy(c->access, key, sizeof(c->access));
            c->refresh[0] = c->username[0] = 0;
        }
    } else if (strcmp(mode, "password") == 0 && user[0] && pass[0]) {
        set_status(true, "Signing in as %s...", user);
        r = abs_login(server, user, pass, c->access, sizeof(c->access), c->refresh, sizeof(c->refresh));
        if (r == ABS_AUTH_OK) strlcpy(c->username, user, sizeof(c->username));
    } else if (c->access[0] && strcmp(server, c->server) == 0) {
        set_status(true, "Checking the saved sign-in...");
        r = abs_check(server, c->access);
        // An expired access token with a refresh token is fine: it renews on first use.
        if (r == ABS_AUTH_REJECTED && c->refresh[0]) r = ABS_AUTH_OK;
    } else {
        set_status(false, strcmp(mode, "apikey") == 0 ? "Enter an API key." : "Enter your username and password.");
        goto out;
    }
    if (r != ABS_AUTH_OK) {
        set_status(false, r == ABS_AUTH_UNREACHABLE ? "Couldn't reach %s. Check the address."
                          : r == ABS_AUTH_REJECTED  ? "The server didn't accept those details."
                                                    : "Unexpected reply from %s.",
                   server);
        goto out;
    }
    strlcpy(c->server, server, sizeof(c->server));

    // Device preferences.
    power_config_t pc = {
        .brightness = num(req, "brightness", 80),
        .screen_off_s = num(req, "screen_off", 60),
        .sleep_min = num(req, "sleep", 10),
    };
    power_set_config(&pc);
    c->skip_back_s = num(req, "skip_back", 30);
    c->skip_fwd_s = num(req, "skip_fwd", 30);
    c->rotate180 = cJSON_IsTrue(cJSON_GetObjectItem(req, "rotate"));

    config_save(c);
    set_status(true, "All set. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(2500));  // let the page show it
    esp_restart();
out:
    free(c);
}

static void apply_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_bring_up) {
            s_bring_up = false;
            if (!s_scan) s_scan = heap_caps_calloc(MAX_SCAN, sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
            s_scan_count = s_scan ? wifi_scan(s_scan, MAX_SCAN) : 0;
            ESP_LOGI(TAG, "%d networks nearby", s_scan_count);
            if (wifi_start_ap(s_ssid, s_pass) == ESP_OK) set_status(false, "Waiting for your phone or laptop");
            else set_status(false, "Couldn't start the setup network.");
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cJSON *req = s_pending;
        s_pending = NULL;
        xSemaphoreGive(s_lock);
        if (req) apply(req);
        cJSON_Delete(req);
    }
}

/* ---------- public ---------- */

void portal_start(void)
{
    if (s_active) return;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "ABSpresso-%02X%02X", mac[4], mac[5]);
    snprintf(s_pass, sizeof(s_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
    // The setup task scans for networks first, then brings the access point up (a scan takes
    // seconds, so keep it off the caller, which holds the UI lock).
    set_status(true, "Starting the setup network...");
    s_scan_count = -1;
    s_bring_up = true;
    if (!s_apply_task) {
        xTaskCreatePinnedToCoreWithCaps(apply_task, "setup", 8192, NULL, 3, &s_apply_task, 0, MALLOC_CAP_SPIRAM);
    }
    xTaskNotifyGive(s_apply_task);

    s_dns_stop = false;
    xTaskCreatePinnedToCoreWithCaps(dns_task, "dns", 4096, NULL, 3, &s_dns_task, 0, MALLOC_CAP_SPIRAM);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // internal RAM is scarce
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 8;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = on_page},
        {.uri = "/api/config", .method = HTTP_GET, .handler = on_config},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = on_scan},
        {.uri = "/api/status", .method = HTTP_GET, .handler = on_status},
        {.uri = "/api/save", .method = HTTP_POST, .handler = on_save},
        {.uri = "/*", .method = HTTP_GET, .handler = on_other},
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    s_active = true;
    ESP_LOGI(TAG, "setup portal on '%s' (internal RAM free %u)", s_ssid,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void portal_stop(void)
{
    if (!s_active) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
    s_dns_stop = true;
    wifi_stop_ap();
    s_active = false;
}

bool portal_active(void)
{
    return s_active;
}

void portal_network(char *ssid, size_t slen, char *pass, size_t plen)
{
    strlcpy(ssid, s_ssid, slen);
    strlcpy(pass, s_pass, plen);
}

void portal_status(char *msg, size_t len, bool *busy)
{
    if (!s_lock) {
        msg[0] = 0;
        *busy = false;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(msg, s_message, len);
    *busy = s_busy;
    xSemaphoreGive(s_lock);
}
