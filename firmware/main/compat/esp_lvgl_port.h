#pragma once
// ABSpresso's shared code takes the LVGL port lock around its ui_* calls. There is no LVGL
// here; these map onto the Yoto UI's own lock (ui.c) so that code compiles unmodified.
#include <stdbool.h>
#include <stdint.h>

bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);
