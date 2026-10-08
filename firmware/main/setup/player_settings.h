#pragma once
// The player's own settings (everything but Wi-Fi and the server sign-in) to and from JSON,
// for the setup page and the player's web page, which show the same fields.
#include "cJSON.h"

void player_settings_to_json(cJSON *o);
// Applies whichever fields are present and valid; the rest are left as they are.
void player_settings_from_json(const cJSON *req);
