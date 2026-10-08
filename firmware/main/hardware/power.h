#pragma once
// Power settings. The interface matches ABSpresso's (its setup portal reads and writes
// these); on the Yoto "screen" means the LED matrix.
//
// Idle dimming, display-off and automatic power-down are not implemented yet: only the
// settings are stored, and brightness is applied.
#include <stdbool.h>

typedef struct {
    int brightness;   // 10-100 %
    int screen_off_s; // 0 = never
    int sleep_min;    // after the screen goes off; 0 = never
} power_config_t;

void power_init(void);
void power_get_config(power_config_t *out);
void power_set_config(const power_config_t *cfg);
// Holds the display on and the device awake (setup does this).
void power_keep_awake(bool on);
