#pragma once
// The Audiobookshelf server and the player's sign-in to it: checked against the server,
// then saved. Changed from the player's web page (confirmed with the code on the display);
// the setup network does the Wi-Fi alone.
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

typedef struct {
    const char *server;    // as typed: https:// is assumed without a scheme
    const char *auth;      // "apikey" or "password"
    const char *username;
    const char *password;
    const char *api_key;
    bool remember;         // keep the password (encrypted) to sign in again by itself
} account_request_t;

// Checks the details with the server and saves them. Blank sign-in details keep the saved
// sign-in, if the server has not changed and it is still accepted. Blocks for as long as
// the server takes (seconds). `msg` says what happened either way.
bool account_apply(const account_request_t *req, char *msg, size_t len);
// A server and a sign-in are saved (whether or not the server still accepts it).
bool account_present(void);
// What the page may show: server, username, auth, signed_in, signed_out, remembered.
// Never a password, key or token.
void account_to_json(cJSON *o);
