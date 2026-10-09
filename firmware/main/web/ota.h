#pragma once
// Updating the firmware over Wi-Fi, from the player's web page, and going back.
//
// The flash has two firmware slots. An update is written to the one not running; at the
// next start the bootloader tries it once ("on trial"). It is kept when it has run for a
// minute, its web page has answered (so another update could be sent to it) and the
// controls and display are running. If it crashes or restarts before then, the bootloader
// starts the previous firmware; if it hangs, or simply never gets there, a timer of its own
// (not the main loop, which may be the thing that is stuck) goes back after five minutes.
//
// A firmware that was kept can still be undone: from the web page, or by holding both knobs
// while switching on. And if a kept firmware crashes three times running soon after
// starting, the player goes back by itself. Going back puts the older firmware on trial in
// turn, so if that one fails, the player returns to the newer.
//
// Updating, going back from the page and formatting the SD card all have to be confirmed at
// the player: the page asks for a code, the player shows four digits on its display, and
// the request is only accepted with them. Nobody can change the firmware from elsewhere on
// the network without seeing the player.
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Call once at start-up, as early as possible after board_init() (before the display, so
// that a firmware that crashes while starting is still caught). Logs which slot is running,
// whether it is on trial, and goes back by itself after repeated crashes.
void ota_init(void);
// Call every time round the main loop. Decides whether a firmware on trial is kept.
void ota_watch(void);

// Shows a new code on the display, good for two minutes and one use.
void ota_new_code(void);
bool ota_code_ok(const char *code);

// An upload, in order: begin, write as many times as needed, then end (true = accepted and
// set to start next) or abort. All may be called from any task.
esp_err_t ota_begin(size_t size);
esp_err_t ota_write(const void *data, size_t len);
esp_err_t ota_end(char *why, size_t why_len);
void ota_abort(void);

// An upload is being written.
bool ota_busy(void);
// "trial" while a new firmware has not yet been kept, else "".
const char *ota_state(void);
// The version in the other slot, if the player can go back to it (a complete image of this
// project that has not failed a trial). False until it has been checked, ten seconds after
// start-up, and while an upload is overwriting it.
bool ota_previous(char *version, size_t len);
// Sets the previous firmware to start, on trial. ESP_OK: the caller restarts the player.
// Otherwise `why` says what is wrong and nothing has changed.
esp_err_t ota_go_back(char *why, size_t why_len);
