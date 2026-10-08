#include "input.h"

#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "board.h"
#include "iox.h"
#include "pins.h"

static const char *TAG = "input";

// To be confirmed on the hardware.
#define LEFT_IS_ENC1        1
#define COUNTS_PER_DETENT   4
#define ENC_REVERSED        1   // the stock config marks both encoders "reverse"

#define POLL_MS             20
#define HOLD_MS             700
#define POWER_HOLD_MS       1500

typedef struct {
    int pin;            // IOX input, low = pressed
    int down_ms;        // how long it has been held; 0 = up
    bool consumed;      // a hold or both-press was already reported for this press
} button_t;

static QueueHandle_t s_events;
static pcnt_unit_handle_t s_enc[2];
static int s_enc_rest[2];  // counts not yet making a whole detent

static esp_err_t encoder_init(int a, int b, pcnt_unit_handle_t *out)
{
    const pcnt_unit_config_t unit_cfg = { .low_limit = -32768, .high_limit = 32767 };
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, out), TAG, "unit");
    const pcnt_glitch_filter_config_t filter = { .max_glitch_ns = 10000 };
    pcnt_unit_set_glitch_filter(*out, &filter);
    pcnt_channel_handle_t cha, chb;
    const pcnt_chan_config_t ca = { .edge_gpio_num = a, .level_gpio_num = b };
    const pcnt_chan_config_t cb = { .edge_gpio_num = b, .level_gpio_num = a };
    ESP_RETURN_ON_ERROR(pcnt_new_channel(*out, &ca, &cha), TAG, "channel A");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(*out, &cb, &chb), TAG, "channel B");
    pcnt_channel_set_edge_action(cha, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(cha, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(chb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(chb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    // Pull-ups where the pin has them (GPIO 35 is input-only and relies on the board's).
    if (a < 34) gpio_set_pull_mode(a, GPIO_PULLUP_ONLY);
    if (b < 34) gpio_set_pull_mode(b, GPIO_PULLUP_ONLY);
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(*out), TAG, "enable");
    pcnt_unit_clear_count(*out);
    return pcnt_unit_start(*out);
}

static void post(ui_event_type_t type, int delta)
{
    static const char *const names[] = { "left turn", "left press", "left hold", "right turn", "right press",
                                         "right hold", "both", "power hold", "card in", "card out",
                                         "power press", "plugged in", "unplugged" };
    ESP_LOGI(TAG, "%s %+d", names[type], delta);  // while the controls are being proven
    const ui_event_t ev = { .type = type, .delta = delta };
    xQueueSend(s_events, &ev, 0);
}

static void poll_encoder(int i, ui_event_type_t type)
{
    int count = 0;
    pcnt_unit_get_count(s_enc[i], &count);
    pcnt_unit_clear_count(s_enc[i]);
    s_enc_rest[i] += ENC_REVERSED ? -count : count;
    const int detents = s_enc_rest[i] / COUNTS_PER_DETENT;
    if (detents) {
        s_enc_rest[i] -= detents * COUNTS_PER_DETENT;
        post(type, detents);
    }
}

static void input_task(void *arg)
{
    button_t left = { .pin = LEFT_IS_ENC1 ? IOX_ENC1_PUSH : IOX_ENC2_PUSH };
    button_t right = { .pin = LEFT_IS_ENC1 ? IOX_ENC2_PUSH : IOX_ENC1_PUSH };
    button_t power = { .pin = IOX_POWER_BUTTON, .consumed = true };  // still held from switching on
    bool plug_seen = false, plug_known = false;
    int plug_steady = 0;
    struct { button_t *b; ui_event_type_t press, hold; int hold_ms; } knobs[] = {
        { &left, UI_EV_L_PRESS, UI_EV_L_HOLD, HOLD_MS },
        { &right, UI_EV_R_PRESS, UI_EV_R_HOLD, HOLD_MS },
    };
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        poll_encoder(LEFT_IS_ENC1 ? 0 : 1, UI_EV_L_TURN);
        poll_encoder(LEFT_IS_ENC1 ? 1 : 0, UI_EV_R_TURN);

        uint16_t in;
        if (iox_read(&in) != ESP_OK) continue;
        const bool l = !iox_bit(in, left.pin), r = !iox_bit(in, right.pin);

        // Both knobs down together is its own gesture and cancels their separate ones.
        if (l && r && !(left.consumed && right.consumed)) {
            left.consumed = right.consumed = true;
            post(UI_EV_BOTH, 0);
        }
        const bool down[2] = { l, r };
        for (int i = 0; i < 2; i++) {
            button_t *b = knobs[i].b;
            if (down[i]) {
                b->down_ms += POLL_MS;
                if (!b->consumed && b->down_ms >= knobs[i].hold_ms) {
                    b->consumed = true;
                    post(knobs[i].hold, 0);
                }
            } else {
                // Two polls down is the debounce.
                if (b->down_ms >= 2 * POLL_MS && !b->consumed) post(knobs[i].press, 0);
                b->down_ms = 0;
                b->consumed = false;
            }
        }

        if (!iox_bit(in, power.pin)) {
            power.down_ms += POLL_MS;
            if (!power.consumed && power.down_ms >= POWER_HOLD_MS) {
                power.consumed = true;
                post(UI_EV_POWER_HOLD, 0);
            }
        } else {
            if (power.down_ms >= 2 * POLL_MS && !power.consumed) post(UI_EV_POWER_PRESS, 0);
            power.down_ms = 0;
            power.consumed = false;
        }

        // External power arriving or going (the charger's plug-status line, low = present),
        // once it has read the same for a tenth of a second. The first reading only sets
        // the starting point.
        // The headphone socket, once it has read the same for a tenth of a second: its
        // contact chatters as a plug goes in or comes out (seen in the log as in/out/in/out
        // within 60 ms).
        static int last_hp = -1, hp_steady;
        const int hp = !iox_bit(in, IOX_HP_DETECT);
        if (hp == last_hp) {
            hp_steady = 0;
        } else if (last_hp < 0 || ++hp_steady >= 5) {
            if (last_hp >= 0) {
                ESP_LOGI(TAG, "headphones %s", hp ? "in" : "out");
                board_headphones_changed();
            }
            last_hp = hp;
            hp_steady = 0;
        }
        static int last_lines = -1;
        const int lines = (iox_bit(in, IOX_PLUG_STATUS) ? 1 : 0) | (iox_bit(in, IOX_CHG_STATUS) ? 2 : 0);
        if (lines != last_lines) {
            ESP_LOGI(TAG, "power lines: plug pin %d, charge pin %d", lines & 1, lines >> 1);
            last_lines = lines;
        }
        const bool plug_now = !iox_bit(in, IOX_PLUG_STATUS);
        if (plug_now == plug_seen) {
            plug_steady = 0;
        } else if (++plug_steady >= 5) {
            plug_steady = 0;
            plug_seen = plug_now;
            if (plug_known) post(plug_now ? UI_EV_PLUGGED : UI_EV_UNPLUGGED, 0);
            plug_known = true;
        }
        if (!plug_known && plug_steady == 0) plug_known = true;
    }
}

esp_err_t input_init(QueueHandle_t *events)
{
    s_events = xQueueCreate(16, sizeof(ui_event_t));
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_ERROR(encoder_init(PIN_ENC1_A, PIN_ENC1_B, &s_enc[0]), TAG, "encoder 1");
    ESP_RETURN_ON_ERROR(encoder_init(PIN_ENC2_A, PIN_ENC2_B, &s_enc[1]), TAG, "encoder 2");
    xTaskCreatePinnedToCore(input_task, "input", 3072, NULL, 4, NULL, 1);
    *events = s_events;
    return ESP_OK;
}
