#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

// Starts the station with the saved network (config.h) and keeps reconnecting. With no network
// saved it stays idle until setup provides one.
esp_err_t wifi_start(void);
bool wifi_wait_connected(int timeout_ms);
bool wifi_is_connected(void);

// Setup portal: bring up our own WPA2 network alongside the station, and take it down again.
esp_err_t wifi_start_ap(const char *ssid, const char *password);
void wifi_stop_ap(void);
// Blocking scan of nearby networks; returns how many were written to `out`. The radio leaves
// its channel for a few seconds, so don't scan while a phone is on the setup network.
int wifi_scan(wifi_ap_record_t *out, int max);
// Joins a network (blocking, up to timeout_ms). Leaves it connected on success.
bool wifi_try_connect(const char *ssid, const char *password, int timeout_ms);
