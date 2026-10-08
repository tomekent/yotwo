#pragma once

// Setup portal: the device's own Wi-Fi network (WPA2, random password shown on screen) with a
// captive web page for Wi-Fi, the Audiobookshelf server and sign-in, and device preferences.
// Saving tests the Wi-Fi and the sign-in first, then stores everything and restarts.

#include <stdbool.h>
#include <stddef.h>

void portal_start(void);
void portal_stop(void);
bool portal_active(void);
// The setup network's name and password (for the on-screen QR code).
void portal_network(char *ssid, size_t slen, char *pass, size_t plen);
// Progress message for the screen, and whether a save is being processed.
void portal_status(char *msg, size_t len, bool *busy);
