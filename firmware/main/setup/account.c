#include "account.h"

#include <stdio.h>
#include <string.h>
#include "abs_api.h"
#include "config.h"
#include "credential.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "account";

bool account_present(void)
{
    const app_config_t *c = config_get();
    return c->server[0] && c->access[0];
}

void account_to_json(cJSON *o)
{
    const app_config_t *c = config_get();
    cJSON_AddStringToObject(o, "server", c->server);
    cJSON_AddStringToObject(o, "username", c->username);
    cJSON_AddStringToObject(o, "auth", (c->access[0] && !c->refresh[0] && !c->username[0]) ? "apikey" : "password");
    cJSON_AddBoolToObject(o, "signed_in", c->access[0] != 0);
    cJSON_AddBoolToObject(o, "signed_out", abs_signed_out());
    cJSON_AddBoolToObject(o, "remembered", credential_present());
}

bool account_apply(const account_request_t *req, char *msg, size_t len)
{
    app_config_t *c = heap_caps_malloc(sizeof(*c), MALLOC_CAP_SPIRAM);
    if (!c) {
        snprintf(msg, len, "The player is out of memory.");
        return false;
    }
    *c = *config_get();
    bool ok = false;

    // Server: trimmed, no trailing slash, https:// assumed if no scheme given.
    char server[128];
    const char *srv = req->server ? req->server : "";
    while (*srv == ' ') srv++;
    if (!srv[0]) {
        snprintf(msg, len, "Enter your Audiobookshelf server address.");
        goto out;
    }
    snprintf(server, sizeof(server), "%s%s", strstr(srv, "://") ? "" : "https://", srv);
    size_t n = strlen(server);
    while (n && (server[n - 1] == '/' || server[n - 1] == ' ')) server[--n] = 0;

    // Sign in: username/password, an API key, or keep the current credential.
    abs_auth_result_t r;
    const char *mode = req->auth ? req->auth : "", *user = req->username ? req->username : "";
    const char *pass = req->password ? req->password : "", *key = req->api_key ? req->api_key : "";
    if (strcmp(mode, "apikey") == 0 && key[0]) {
        r = abs_check(server, key);
        if (r == ABS_AUTH_OK) {
            strlcpy(c->access, key, sizeof(c->access));
            c->refresh[0] = c->username[0] = 0;
            credential_clear();  // a key needs no password
        }
    } else if (strcmp(mode, "password") == 0 && user[0] && pass[0]) {
        r = abs_login(server, user, pass, c->access, sizeof(c->access), c->refresh, sizeof(c->refresh));
        if (r == ABS_AUTH_OK) {
            strlcpy(c->username, user, sizeof(c->username));
            // Kept (encrypted) so the player can sign in again when the server ends its session.
            if (req->remember) credential_save(pass);
            else credential_clear();
        }
    } else if (c->access[0] && strcmp(server, c->server) == 0) {
        r = abs_check(server, c->access);
        // An expired access token with a refresh token is fine: it renews on first use.
        // Not so once the server has refused the renewal too.
        if (r == ABS_AUTH_REJECTED && c->refresh[0] && !abs_signed_out()) r = ABS_AUTH_OK;
        if (r == ABS_AUTH_REJECTED) {
            snprintf(msg, len, "The saved sign-in is no longer accepted. Enter an API key, or your username and password.");
            goto out;
        }
    } else {
        snprintf(msg, len, "%s", strcmp(mode, "apikey") == 0 ? "Enter an API key." : "Enter your username and password.");
        goto out;
    }
    if (r != ABS_AUTH_OK) {
        if (r == ABS_AUTH_UNREACHABLE) snprintf(msg, len, "Couldn't reach %s. Check the address.", server);
        else if (r == ABS_AUTH_REJECTED) snprintf(msg, len, "The server didn't accept those details.");
        else snprintf(msg, len, "Unexpected reply from %s.", server);
        goto out;
    }
    strlcpy(c->server, server, sizeof(c->server));
    config_save(c);
    ESP_LOGI(TAG, "signed in to %s%s%s", server, c->username[0] ? " as " : " with an API key", c->username);
    snprintf(msg, len, "Signed in to %s.", server);
    ok = true;
out:
    free(c);
    return ok;
}
