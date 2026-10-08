#pragma once
// The server password, kept so the player can sign in again by itself when the server has
// ended its session (see "Staying signed in" in docs/firmware.md).
//
// It has to be recoverable, since signing in needs the password itself, so it cannot be
// hashed. It is stored encrypted (AES-256) under a key made from this chip's own address:
// a copy of the flash read on another machine does not show it as text, but anyone who has
// both the flash contents and this source can recover it. That is the most that can be done
// without the ESP32's flash encryption, which is a one-way change to the chip.
#include <stdbool.h>
#include <stddef.h>

void credential_save(const char *password);
void credential_clear(void);
bool credential_present(void);
// Copies the password out; false if none is kept. The caller should wipe it after use.
bool credential_load(char *out, size_t len);
