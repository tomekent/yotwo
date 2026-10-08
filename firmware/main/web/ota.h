#pragma once
// Updating the firmware over Wi-Fi, from the player's web page.
//
// The flash has two firmware slots. An update is written to the one not running; at the
// next start the bootloader tries it once. If it gets as far as serving the web page (so
// that another update could be sent to it) it is kept; if it crashes, hangs or cannot get
// that far within five minutes, the player restarts into the previous firmware, which is
// still in the other slot.
//
// An update has to be confirmed at the player: the page asks for a code, the player shows
// four digits on its display, and the upload is only accepted with them. Nobody can change
// the firmware from elsewhere on the network without seeing the player.
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Call once at start-up (after the UI is up). Says in the log which slot is running and
// whether it is on trial.
void ota_init(void);
// Call every so often from the main loop with whether the web page is being served.
// Confirms a firmware on trial, or restarts out of one that has not proved itself.
void ota_watch(bool web_up);

// Shows a new code on the display, good for two minutes and one upload.
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
// "trial" while a new firmware has not yet been confirmed, else "".
const char *ota_state(void);
