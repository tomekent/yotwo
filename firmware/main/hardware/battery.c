#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "iox.h"
#include "pins.h"

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static int s_last_mv;

// Percentage from the resting voltage of a single lithium cell, by straight lines between
// these points. (The stock firmware reports only 0, 10, 20, 33, 66 and 100; this is finer
// so that a level bar means something.) Our own estimate, not measured on this cell.
static int percent_from(int mv)
{
    static const struct { int mv, pct; } curve[] = {
        { 3300, 0 }, { 3500, 8 }, { 3600, 18 }, { 3700, 38 }, { 3800, 58 }, { 3950, 80 }, { 4100, 100 },
    };
    const int n = sizeof(curve) / sizeof(curve[0]);
    if (mv <= curve[0].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv < curve[i].mv) {
            return curve[i - 1].pct +
                   (mv - curve[i - 1].mv) * (curve[i].pct - curve[i - 1].pct) / (curve[i].mv - curve[i - 1].mv);
        }
    }
    return 100;
}

void battery_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "ADC unavailable");
        s_adc = NULL;
        return;
    }
    const adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    adc_oneshot_config_channel(s_adc, ADC_CH_BATTERY, &chan);
    const adc_cali_line_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
        .default_vref = 1093,  // the stock firmware's fallback; the eFuse value (1121 mV) wins
    };
    if (adc_cali_create_scheme_line_fitting(&cali, &s_cali) != ESP_OK) s_cali = NULL;
}

void battery_get(battery_status_t *out)
{
    *out = (battery_status_t){ 0 };
    if (!s_adc || !s_cali) return;
    // A few readings averaged: single ones jump about.
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, pin_mv;
        if (adc_oneshot_read(s_adc, ADC_CH_BATTERY, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, raw, &pin_mv) == ESP_OK) {
            sum += pin_mv;
            n++;
        }
    }
    if (!n) return;
    const int mv = (sum / n) * 82 / 39;
    uint16_t pins = 0xFFFF;
    const bool pins_ok = iox_read(&pins) == ESP_OK;
    static bool last_plugged = true;  // until read: a failed read must not look like "on battery"
    const bool plugged = pins_ok ? !iox_bit(pins, IOX_PLUG_STATUS) : last_plugged;
    last_plugged = plugged;
    const bool charging_pin = pins_ok && !iox_bit(pins, IOX_CHG_STATUS);

    // On external power the voltage is the charger's, not the cell's (about 4.3 V as soon
    // as the cable goes in), so it says nothing of the level. Then the level is the last
    // one measured on the battery, or full once the charger reports it has finished.
    static bool was_plugged;
    static int s_cell_percent = -1;
    if (plugged != was_plugged) s_last_mv = 0;  // no averaging across the change
    was_plugged = plugged;
    s_last_mv = s_last_mv ? (s_last_mv + mv) / 2 : mv;  // smoothed as the stock firmware does
    out->volts = s_last_mv / 1000.0f;
    out->present = s_last_mv > 2500 && s_last_mv < 4600;
    if (!plugged) {
        s_cell_percent = out->percent = percent_from(s_last_mv);
    } else if (!charging_pin) {
        s_cell_percent = out->percent = 100;
    } else {
        out->percent = s_cell_percent >= 0 ? s_cell_percent : percent_from(s_last_mv);
    }
    out->charging = plugged && charging_pin;
    out->charged = plugged && !charging_pin;

    static int64_t last_log;
    const int64_t now = esp_timer_get_time();
    if (!last_log || now - last_log > 60 * 1000000LL) {
        last_log = now;
        ESP_LOGI(TAG, "%d mV, %d%%; plug pin %d, charge pin %d -> %s", s_last_mv, out->percent,
                 iox_bit(pins, IOX_PLUG_STATUS), iox_bit(pins, IOX_CHG_STATUS),
                 out->charging ? "charging" : out->charged ? "charged" : "on battery");
    }
}
