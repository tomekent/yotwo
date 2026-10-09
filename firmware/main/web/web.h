#pragma once
// The player's own web page on the home network: http://<name>.local (toms-yotwo.local for
// a player named "Tom's Yotwo"). What is playing, the battery and the network; every setting
// but Wi-Fi (that stays with SET > WIFI); the server and sign-in (confirmed with a code on
// the display); the copies kept on the SD card, with their progress and a way to remove them;
// and, for finding faults, the SD card's files to look at.
//
// There is no password: anyone on the home network can open it. Nothing it shows includes
// a password, key or token, and changing the sign-in needs the code on the display.
#include <stdbool.h>
#include <stddef.h>

// Announces the name on the network (mDNS). Call once, after wifi_start().
void web_init(void);
// Starts the page's server if it is not running. Call when Wi-Fi is up and setup is not.
void web_start(void);
// Whether the page is being served.
bool web_running(void);
// Whether the page has answered a request since start-up. If not, and it is running, asks
// it for the player's state (at most every 15 s; blocks for up to 5 s).
bool web_check(void);
// Stops it (the setup page needs port 80).
void web_stop(void);
// The player's name has changed: announce the new address.
void web_name_changed(void);
// The address made from the player's name, without ".local": lower case, digits and dashes.
void web_hostname(char *out, size_t len);
