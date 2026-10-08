#include "ring.h"

#include <math.h>
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pins.h"

static const char *TAG = "ring";

// Whether a channel lights when its pin is low. Not known from the stock config; if the
// ring is lit at start-up with nothing asked of it, this is the wrong way round.
#define RING_ACTIVE_LOW 0
#define RING_BITS       10
#define RING_MAX        ((1 << RING_BITS) - 1)

static bool s_ready;
static uint32_t s_last = 0xFFFFFFFF;

esp_err_t ring_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = RING_BITS,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "timer");
    const int pins[3] = { PIN_NL_RED, PIN_NL_GREEN, PIN_NL_BLUE };
    for (int i = 0; i < 3; i++) {
        const ledc_channel_config_t ch = {
            .gpio_num = pins[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)i,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .flags.output_invert = RING_ACTIVE_LOW,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "channel %d", i);
    }
    s_ready = true;
    ring_set(0, 0, 0);
    ESP_LOGI(TAG, "LED ring ready (off)");
    return ESP_OK;
}

static uint32_t duty_for(uint8_t v)
{
    // Perceived brightness to PWM: LEDs look far brighter at low duty than they are.
    const float x = v / 255.0f;
    return (uint32_t)(powf(x, 2.2f) * RING_MAX + 0.5f);
}

void ring_set(uint8_t r, uint8_t g, uint8_t b)
{
    const uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (!s_ready || rgb == s_last) return;
    s_last = rgb;
    const uint8_t v[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, duty_for(v[i]));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    }
    ESP_LOGI(TAG, "ring %02X%02X%02X", r, g, b);
}
