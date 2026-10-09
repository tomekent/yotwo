#include "player_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ui_host.h"

static int num(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

void player_settings_to_json(cJSON *o)
{
    cJSON_AddStringToObject(o, "name", ui_player_name());
    cJSON_AddBoolToObject(o, "covers", host_cover_view());
    cJSON_AddNumberToObject(o, "speed", host_scroll_speed());
    cJSON_AddBoolToObject(o, "readable", host_readable());
    cJSON_AddNumberToObject(o, "extend", host_sleep_extend());
    cJSON_AddStringToObject(o, "tz", host_timezone());
    cJSON_AddNumberToObject(o, "auto_off", host_auto_off());
    cJSON_AddNumberToObject(o, "vol_limit", host_volume_limit());
    cJSON_AddNumberToObject(o, "bright_day", host_bright_setting(UI_BRIGHT_DAY));
    cJSON_AddNumberToObject(o, "bright_night", host_bright_setting(UI_BRIGHT_NIGHT));
    cJSON_AddBoolToObject(o, "clock24", host_clock_setting(UI_CLOCK_24H));
    cJSON_AddNumberToObject(o, "rise", host_clock_setting(UI_CLOCK_RISE));
    cJSON_AddNumberToObject(o, "dusk", host_clock_setting(UI_CLOCK_SET));
    cJSON_AddNumberToObject(o, "fade", host_clock_setting(UI_CLOCK_FADE));
    char rgb[8];
    snprintf(rgb, sizeof(rgb), "#%06X", (unsigned)host_light_setting(UI_LIGHT_COLOUR) & 0xFFFFFF);
    cJSON_AddBoolToObject(o, "night", host_light_setting(UI_LIGHT_NIGHT));
    cJSON_AddStringToObject(o, "night_color", rgb);
    cJSON_AddNumberToObject(o, "night_bright", host_light_setting(UI_LIGHT_BRIGHT));
    cJSON_AddBoolToObject(o, "wake", host_light_setting(UI_LIGHT_WAKE));
    cJSON_AddNumberToObject(o, "wake_hold", host_light_setting(UI_LIGHT_HOLD));
}

void player_settings_from_json(const cJSON *req)
{
    const cJSON *name = cJSON_GetObjectItem(req, "name");
    if (cJSON_IsString(name)) ui_set_player_name(name->valuestring);
    const cJSON *covers = cJSON_GetObjectItem(req, "covers"), *readable = cJSON_GetObjectItem(req, "readable");
    if (cJSON_IsBool(covers)) host_set_cover_view(cJSON_IsTrue(covers));
    if (cJSON_IsBool(readable)) host_set_readable(cJSON_IsTrue(readable));
    const int speed = num(req, "speed", 0), extend = num(req, "extend", 0);
    if (speed >= 3 && speed <= 30) host_set_scroll_speed(speed);
    if (extend >= 1 && extend <= 60) host_set_sleep_extend(extend);
    // The clock. Sunrise must leave room for both fades before sunset, or it is left alone.
    const cJSON *clock24 = cJSON_GetObjectItem(req, "clock24");
    if (cJSON_IsBool(clock24)) host_set_clock_setting(UI_CLOCK_24H, cJSON_IsTrue(clock24));
    const int rise = num(req, "rise", -1), dusk = num(req, "dusk", -1), fade = num(req, "fade", -1);
    if (rise >= 0 && dusk < 24 * 60 && fade >= 1 && fade <= 120 && rise + 2 * fade <= dusk) {
        host_set_clock_setting(UI_CLOCK_RISE, rise);
        host_set_clock_setting(UI_CLOCK_SET, dusk);
        host_set_clock_setting(UI_CLOCK_FADE, fade);
    }
    const cJSON *tz = cJSON_GetObjectItem(req, "tz");
    if (cJSON_IsString(tz)) host_set_timezone(tz->valuestring);
    const int auto_off = num(req, "auto_off", -1);
    if (auto_off >= 0) host_set_auto_off(auto_off);
    host_set_volume_limit(num(req, "vol_limit", 0));  // ignored unless 10-100
    host_set_bright_setting(UI_BRIGHT_DAY, num(req, "bright_day", 0));      // likewise
    host_set_bright_setting(UI_BRIGHT_NIGHT, num(req, "bright_night", 0));
    // The LED ring. Values out of range are ignored by the setters.
    const cJSON *night = cJSON_GetObjectItem(req, "night"), *wake = cJSON_GetObjectItem(req, "wake");
    if (cJSON_IsBool(night)) host_set_light_setting(UI_LIGHT_NIGHT, cJSON_IsTrue(night));
    if (cJSON_IsBool(wake)) host_set_light_setting(UI_LIGHT_WAKE, cJSON_IsTrue(wake));
    const cJSON *colour = cJSON_GetObjectItem(req, "night_color");
    if (cJSON_IsString(colour) && colour->valuestring[0] == '#' && strlen(colour->valuestring) == 7) {
        host_set_light_setting(UI_LIGHT_COLOUR, (int)strtol(colour->valuestring + 1, NULL, 16));
    }
    host_set_light_setting(UI_LIGHT_BRIGHT, num(req, "night_bright", 0));
    host_set_light_setting(UI_LIGHT_HOLD, num(req, "wake_hold", 0));
}
