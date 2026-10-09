// Yoto Player Gen 2 as an Audiobookshelf player: ABSpresso's network and app code on the
// Yoto's hardware, with the two-knob UI. NFC cards are not handled yet.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "gfx.h"
#include "nvs_flash.h"
#include "abs_api.h"
#include "account.h"
#include "battery.h"
#include "accel.h"
#include "board.h"
#include "ring.h"
#include "cards.h"
#include "catalog.h"
#include "config.h"
#include "cover16.h"
#include "display.h"
#include "download.h"
#include "favourites.h"
#include "input.h"
#include "names.h"
#include "nfc.h"
#include "player.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "offline.h"
#include "portal.h"
#include "power.h"
#include "storage.h"
#include "ui.h"
#include "ota.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "main";

#define RETRY_US (30 * 1000000LL)
#define REFRESH_US (30 * 60 * 1000000LL)  // how often an idle player reloads the library

static void on_time_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "time received from the network");
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    static bool started;
    if (!started) {
        started = true;
        esp_netif_sntp_start();
    } else {
        esp_sntp_restart();  // back on the network: ask again now, not at the next interval
    }
    ESP_LOGI(TAG, "asking the network for the time");
}

// Setup: the device's own open Wi-Fi network, with the setup page's 4-digit code on the display.
static void run_setup(void)
{
    char ssid[33], pass[16];
    portal_start();
    portal_network(ssid, sizeof(ssid), pass, sizeof(pass));
    ESP_LOGI(TAG, "setup network '%s' (open), code %s", ssid, pass);
    ui_show_setup(pass);
}

// On the original ESP32 a 20 KB corner of instruction RAM is offered as heap. It only
// supports aligned 32-bit access, yet it is the first place heap_caps_malloc() looks for a
// plain MALLOC_CAP_INTERNAL request; copying bytes into such a block raises LoadStoreError.
// ABSpresso's player did exactly that (since fixed there by adding MALLOC_CAP_8BIT). This
// uses the region up at start-up so that no other code, ours or a library's, can fall into
// the same trap. The memory is of no use to us otherwise.
static void reserve_iram_heap(void)
{
    size_t taken = 0;
    for (size_t size = 16384; size >= 16; size /= 2) {
        for (;;) {
            void *p = heap_caps_malloc(size, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
            if (!p) break;
            if ((uintptr_t)p >= 0x400A0000) {  // spilled into the shared D/IRAM region: not wanted
                heap_caps_free(p);
                break;
            }
            taken += size;
        }
    }
    ESP_LOGI(TAG, "reserved %u bytes of IRAM heap", (unsigned)taken);
}

// Both knobs held while switching on, for three seconds: back to the firmware before this
// one. For a firmware that was kept but turns out to be no good, when the web page is no
// use. Before the controls start, so the hold is not also taken as a button press.
static void offer_going_back(void)
{
    if (!input_knobs_held()) return;
    gfx_clear();
    gfx_text_centered("BACK", 5, GFX_ORANGE);
    gfx_flush();
    int held_ms = 0;
    while (input_knobs_held() && held_ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        held_ms += 50;
    }
    char why[64];
    if (held_ms >= 3000) {
        if (ota_go_back(why, sizeof(why)) == ESP_OK) {
            gfx_clear();
            gfx_text_centered("BACK", 5, GFX_GREEN);
            gfx_flush();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        ESP_LOGW(TAG, "both knobs at start-up, but %s", why);
        gfx_clear();
        gfx_text_centered("NONE", 5, GFX_RED);
        gfx_flush();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    while (input_knobs_held()) vTaskDelay(pdMS_TO_TICKS(50));
    gfx_clear();
    gfx_flush();
}

void app_main(void)
{
    reserve_iram_heap();
    // The power latch first: on battery, releasing the button before this runs switches off.
    const esp_err_t board_err = board_init();
    // Straight after: a firmware that crashes while starting must still be counted.
    ota_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    config_init();
    nvs_stats_t nvs;
    if (nvs_get_stats(NULL, &nvs) == ESP_OK) {
        ESP_LOGI(TAG, "settings store: %u of %u entries used, %u free", (unsigned)nvs.used_entries,
                 (unsigned)nvs.total_entries, (unsigned)nvs.free_entries);
    }
    ESP_LOGI(TAG, "reset reason %d, board init %s, PSRAM %u KB", (int)esp_reset_reason(),
             esp_err_to_name(board_err), (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_ERROR_CHECK(board_err);

    ESP_ERROR_CHECK(display_init());
    offer_going_back();
    ring_init();
    accel_init(board_i2c_bus());  // carries on without the face-down nightlight if missing
    QueueHandle_t events = NULL;
    if (input_init(&events) != ESP_OK) {
        ESP_LOGE(TAG, "no controls");
        events = NULL;
    }
    ui_init(events);
    if (events) nfc_init(events);  // carries on without cards if the reader is silent
    ui_show_status("WIFI", 0xFFD166);

    ESP_ERROR_CHECK(board_audio_init());
    storage_init();
    cards_init();
    names_init();
    battery_init();
    power_init();
    abs_api_init();
    cover16_init();
    favourites_init();
    player_init();
    download_init();
    offline_init();
    wifi_start();
    // The time, for the clock. It is asked for the moment the network gives us an address
    // (on_got_ip): started any earlier, the first request fails for want of a network and
    // the next is not made for 15 s, then 30, then 60.
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.start = false;
    sntp.sync_cb = on_time_sync;
    esp_netif_sntp_init(&sntp);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    web_init();
    catalog_init();

    if (!config_get()->wifi_ssid[0]) {
        run_setup();  // first run, or settings cleared; saving restarts the device
    }
    bool cached = catalog_load_cached();
    bool online = false;
    int64_t last_try = -RETRY_US;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        // The player's own web page, whenever the home network is there and setup is not.
        // Before the library load below, which can take twenty seconds.
        if (wifi_is_connected() && !portal_active()) web_start();
        ota_watch();
        // No server or sign-in yet (they are set on the web page, not in setup): say where.
        static bool told_web;
        if (!account_present() && wifi_is_connected() && web_running() && !told_web) {
            told_web = true;
            char host[48], msg[96];
            web_hostname(host, sizeof(host));
            snprintf(msg, sizeof(msg), "SIGN IN AT %s.LOCAL", host);
            ESP_LOGW(TAG, "no server or sign-in yet: open http://%s.local", host);
            ui_show_message_locked(msg);
        }
        if (!portal_active() && !online && account_present() && !abs_signed_out() && wifi_is_connected() &&
            now - last_try >= RETRY_US) {
            ui_show_status("SYNC", 0x40E0D0);
            online = catalog_load_network(cached);
            last_try = now;
            if (online) {
                ESP_LOGI(TAG, "free after library load: internal %u, PSRAM %u",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            }
        }

        char library[40];
        if (ui_take_library_request(library, sizeof(library))) {
            // Switch library: show its cache at once (if any), then refresh from the server.
            char previous[40];
            strlcpy(previous, catalog_library_id(), sizeof(previous));
            ESP_LOGI(TAG, "switching to library %s", library);
            catalog_select(library);
            cached = catalog_load_cached();
            if (wifi_is_connected()) {
                online = catalog_load_network(cached);
                last_try = now;
            }
            if (!cached && !online) {
                catalog_select(previous);  // nothing to show: stay where we were
                catalog_load_cached();
            }
            ui_library_request_done();
        }
        // Wi-Fi back after being away: reload now rather than at the next half hour. That
        // sends the server whatever was listened to offline and brings back everything else.
        static bool lost;
        const bool connected = wifi_is_connected();
        if (!connected && online) lost = true;  // (not the wait for it at start-up)
        if (connected && lost) {
            lost = false;
            ESP_LOGI(TAG, "Wi-Fi is back");
            last_try = now - REFRESH_US;
        }
        // Now and then, while nothing is playing, ask the server again: what was listened to
        // elsewhere, new books, and changes to the Favourites collection (which decides
        // what is kept on the card) would otherwise wait for the next switch-on.
        player_status_t ps;
        player_get_status(&ps);
        const bool playing = ps.state == PLAYER_LOADING || ps.state == PLAYER_BUFFERING || ps.state == PLAYER_PLAYING;
        if (online && !playing && !portal_active() && !abs_signed_out() && wifi_is_connected() &&
            now - last_try >= REFRESH_US) {
            last_try = now;
            ESP_LOGI(TAG, "refreshing the library");
            catalog_load_network(true);
        }
        // The server has refused our sign-in and it could not be renewed: say so, once.
        static bool told;
        if (abs_signed_out() && !told) {
            told = true;
            ESP_LOGW(TAG, "signed out by the server");
            char host[48], msg[96];
            web_hostname(host, sizeof(host));
            snprintf(msg, sizeof(msg), "SIGNED OUT - SIGN IN AGAIN AT %s.LOCAL", host);
            ui_show_message_locked(msg);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
