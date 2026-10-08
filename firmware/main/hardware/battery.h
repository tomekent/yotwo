#pragma once
// Battery level from the cell voltage on ADC1 channel 3 (battery mV = pin mV * 82 / 39),
// and the charger's two status lines on the IO expander. The interface matches ABSpresso's.
//
// The status lines are taken to be active low, as the stock firmware reads them (it
// inverts both, and enables pull-ups on them): plug status low = external power present,
// charge status low = charging. To be confirmed on the player; battery_get() logs the raw
// levels now and then for that purpose.
#include <stdbool.h>

typedef struct {
    bool present;     // false if the reading is implausible (no battery / sense line)
    float volts;
    int percent;      // 0-100 from the voltage (reads high while charging)
    bool charging;    // on external power, still charging
    bool charged;     // on external power, charging finished
} battery_status_t;

void battery_init(void);
void battery_get(battery_status_t *out);
