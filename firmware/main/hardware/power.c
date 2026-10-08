#include "power.h"

#include "config.h"
#include "display.h"
#include "nvs.h"

static power_config_t s_cfg = { .brightness = 40, .screen_off_s = 60, .sleep_min = 10 };

void power_init(void)
{
    nvs_handle_t h;
    if (nvs_open("power", NVS_READONLY, &h) == ESP_OK) {
        uint8_t b;
        uint16_t off, sleep;
        if (nvs_get_u8(h, "bright", &b) == ESP_OK) s_cfg.brightness = b;
        if (nvs_get_u16(h, "off", &off) == ESP_OK) s_cfg.screen_off_s = off;
        if (nvs_get_u16(h, "sleep", &sleep) == ESP_OK) s_cfg.sleep_min = sleep;
        nvs_close(h);
    }
    display_set_brightness(s_cfg.brightness);
}

void power_get_config(power_config_t *out)
{
    *out = s_cfg;
}

static void save(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("power", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", s_cfg.brightness);
        nvs_set_u16(h, "off", s_cfg.screen_off_s);
        nvs_set_u16(h, "sleep", s_cfg.sleep_min);
        nvs_commit(h);
        nvs_close(h);
    }
}

void power_set_config(const power_config_t *cfg)
{
    s_cfg = *cfg;
    display_set_brightness(s_cfg.brightness);
    flash_safe(save, NULL);  // callers may be on PSRAM stacks
}

void power_keep_awake(bool on)
{
    (void)on;
}
