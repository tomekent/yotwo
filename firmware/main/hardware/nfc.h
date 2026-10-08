#pragma once
// The card reader: an ST CR95HF on the shared SPI bus, with its chip select and wake line
// on the IO expander. Reads the UID of an ISO 14443-A card (Yoto cards and NTAG-type tags)
// and reports cards arriving and leaving.
//
// First version ("spike"): proves the reader on the hardware. It logs what the chip sends
// back so the details that could not be settled from the stock firmware can be.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#define NFC_UID_MAX 7

// Needs board_init() first. Posts UI_EV_CARD_IN / UI_EV_CARD_OUT (ui_event_t) to `events`.
// Fails, leaving the rest of the player unaffected, if the reader does not answer.
esp_err_t nfc_init(QueueHandle_t events);
// The card in the slot: copies its UID and returns its length (4 or 7), or 0 if none.
int nfc_uid(uint8_t uid[NFC_UID_MAX]);
// The same as text, e.g. "04A1B2C3D4E5F6"; "" if none. `out` needs 15 bytes.
void nfc_uid_text(char *out, size_t len);
