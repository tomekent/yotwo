#pragma once
// The two rotary encoders, their push buttons and the power button, turned into UI events.
//
// Not yet established on hardware, so set by the constants at the top of input.c: which
// encoder is the left knob, how many quadrature counts make one detent, and that the knob
// buttons read low when pressed (as the power button does).
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "ui_host.h"

// Starts polling. Events (ui_event_t) are posted to the returned queue.
esp_err_t input_init(QueueHandle_t *events);
// Both knob buttons are down now. Works from board_init() on, before input_init().
bool input_knobs_held(void);
