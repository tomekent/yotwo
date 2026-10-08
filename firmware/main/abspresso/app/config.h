#pragma once

// Device configuration, stored in NVS and set up from a phone through the setup portal
// (portal.c). Nothing is built in: a new device starts in setup.

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server[128];     // base URL, e.g. https://abs.example.com or http://192.168.1.5:13378
    char username[64];    // who signed in (display only; "" when using an API key)
    char access[1024];    // Bearer credential: an API key, or an access token from signing in
    char refresh[1024];   // refresh token for renewing the access token ("" for API keys)
    int skip_back_s;      // Now Playing skip buttons
    int skip_fwd_s;
    bool rotate180;       // screen upside down (how this device is usually mounted)
} app_config_t;

void config_init(void);
// Read-only view; valid for the life of the program (contents change only via the setters).
const app_config_t *config_get(void);
// Replaces and saves the whole configuration.
void config_save(const app_config_t *cfg);
// Saves new tokens after a refresh (thread-safe).
void config_set_tokens(const char *access, const char *refresh);
// Saves the screen rotation preference.
void config_set_rotate(bool rotate180);
// Saves the Now Playing skip lengths (seconds).
void config_set_skip(int back_s, int fwd_s);
// Wi-Fi, a server and a credential are all present.
bool config_complete(void);

// Runs fn(arg) where flash (NVS) access is allowed, and waits for it. Flash operations disable
// the cache PSRAM sits behind, so a task with a PSRAM stack can't do them itself: from such a
// task this borrows a short-lived task with an internal-RAM stack.
void flash_safe(void (*fn)(void *), void *arg);
