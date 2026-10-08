#include "nfc.h"

#include <stdio.h>
#include <string.h>
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "iox.h"
#include "pins.h"
#include "ui_host.h"

static const char *TAG = "nfc";

// SPI control bytes (the first byte of every exchange).
#define CTRL_SEND  0x00
#define CTRL_RESET 0x01
#define CTRL_READ  0x02
#define CTRL_POLL  0x03
#define POLL_READY 0x08   // flag: a response can be read

// Commands.
#define CMD_PROTOCOL 0x02
#define CMD_SENDRECV 0x04
#define CMD_ECHO     0x55

// Result codes.
#define RES_DATA     0x80  // a card answered
#define RES_TIMEOUT  0x87  // nothing answered

#define POLL_MS      500
#define MISSES_GONE  3     // consecutive empty polls before a card counts as removed
#define BUF          40

static spi_device_handle_t s_spi;
static uint8_t *s_tx, *s_rx;          // DMA-capable
static QueueHandle_t s_events;
static SemaphoreHandle_t s_lock;
static uint8_t s_uid[NFC_UID_MAX];
static int s_uid_len;
static int s_log_budget = 6;          // raw exchanges still to be logged at start-up

// One chip-select framed exchange of `len` bytes, s_tx out and s_rx in.
static esp_err_t xfer(int len)
{
    spi_transaction_t t = { .length = len * 8, .tx_buffer = s_tx, .rx_buffer = s_rx };
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_spi, portMAX_DELAY), TAG, "bus");
    iox_set(IOX_NFC_CS, 0);
    const esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    iox_set(IOX_NFC_CS, 1);
    spi_device_release_bus(s_spi);
    return err;
}

static void log_raw(const char *what, const uint8_t *p, int n)
{
    if (s_log_budget <= 0) return;
    s_log_budget--;
    char hex[3 * 20 + 1] = "";
    for (int i = 0; i < n && i < 20; i++) sprintf(hex + 3 * i, "%02X ", p[i]);
    ESP_LOGI(TAG, "%s: %s", what, hex);
}

// Sends a command and reads the reply. Returns the result code (the first reply byte) and
// copies up to `max` data bytes into `data`, setting *n; or -1 if the chip did not answer
// within `wait_ms`. Echo is the one command whose reply is a single byte with no length.
static int command(uint8_t cmd, const uint8_t *params, int plen, uint8_t *data, int max, int *n, int wait_ms)
{
    if (n) *n = 0;
    memset(s_tx, 0, BUF);
    s_tx[0] = CTRL_SEND;
    s_tx[1] = cmd;
    int len = 2;
    if (cmd != CMD_ECHO) {
        s_tx[2] = plen;
        memcpy(s_tx + 3, params, plen);
        len = 3 + plen;
    }
    if (xfer(len) != ESP_OK) return -1;

    // Wait until the chip says a reply is ready.
    bool ready = false;
    for (int waited = 0; waited <= wait_ms && !ready; waited += 2) {
        memset(s_tx, CTRL_POLL, 2);
        if (xfer(2) != ESP_OK) return -1;
        ready = s_rx[1] & POLL_READY;
        if (!ready) vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!ready) return -1;

    // Read it: after the control byte come the result code, the data length and the data.
    memset(s_tx, 0, BUF);
    s_tx[0] = CTRL_READ;
    if (xfer(BUF) != ESP_OK) return -1;
    log_raw(cmd == CMD_ECHO ? "echo" : cmd == CMD_PROTOCOL ? "protocol" : "reply", s_rx, 16);
    const int code = s_rx[1];
    if (cmd == CMD_ECHO) return code;
    int dlen = s_rx[2];
    if (dlen > BUF - 3) dlen = BUF - 3;
    if (data) {
        const int copy = dlen < max ? dlen : max;
        memcpy(data, s_rx + 3, copy);
        if (n) *n = copy;
    }
    return code;
}

static bool reader_start(void)
{
    // Wake pulse on IRQ_IN, which also tells the chip to use SPI, then a reset.
    iox_set(IOX_NFC_IRQ_IN, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    iox_set(IOX_NFC_IRQ_IN, 1);
    vTaskDelay(pdMS_TO_TICKS(15));
    s_tx[0] = CTRL_RESET;
    xfer(1);
    vTaskDelay(pdMS_TO_TICKS(5));
    iox_set(IOX_NFC_IRQ_IN, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    iox_set(IOX_NFC_IRQ_IN, 1);
    vTaskDelay(pdMS_TO_TICKS(15));

    bool alive = false;
    for (int i = 0; i < 5 && !alive; i++) {
        alive = command(CMD_ECHO, NULL, 0, NULL, 0, NULL, 50) == CMD_ECHO;
    }
    if (!alive) return false;
    static const uint8_t iso14443a[] = { 0x02, 0x00 }, off[] = { 0x00, 0x00 };
    if (command(CMD_PROTOCOL, iso14443a, sizeof(iso14443a), NULL, 0, NULL, 50) != 0x00) return false;
    command(CMD_PROTOCOL, off, sizeof(off), NULL, 0, NULL, 50);  // field off until the first look
    return true;
}

// A short frame to the card: `data` then the transmit-flags byte.
static int to_card(const uint8_t *frame, int flen, uint8_t *reply, int max, int *n)
{
    return command(CMD_SENDRECV, frame, flen, reply, max, n, 60);
}

// The reader's radio field. With it left on all the time the speaker crackled, so it is
// switched on only for the few milliseconds of each look for a card.
static void field(bool on)
{
    static const uint8_t iso14443a[] = { 0x02, 0x00 }, off[] = { 0x00, 0x00 };
    command(CMD_PROTOCOL, on ? iso14443a : off, 2, NULL, 0, NULL, 50);
    if (on) vTaskDelay(pdMS_TO_TICKS(10));  // a card needs a moment to power up
}

static int read_card_field_on(uint8_t uid[NFC_UID_MAX]);

// Is a card there? If so, its UID.
static int read_card(uint8_t uid[NFC_UID_MAX])
{
    field(true);
    const int len = read_card_field_on(uid);
    field(false);
    return len;
}

static int read_card_field_on(uint8_t uid[NFC_UID_MAX])
{
    static const uint8_t wupa[] = { 0x52, 0x07 };            // 7-bit frame
    static const uint8_t anticoll1[] = { 0x93, 0x20, 0x08 };
    static const uint8_t anticoll2[] = { 0x95, 0x20, 0x08 };
    uint8_t r[16];
    int n;
    // A card left part-way through the last exchange ignores one wake-up and answers the next.
    // Three tries: with the field only just switched on, a card that is there sometimes
    // misses the first (the player twice reported a card gone and back within a second).
    if (to_card(wupa, sizeof(wupa), r, sizeof(r), &n) != RES_DATA &&
        to_card(wupa, sizeof(wupa), r, sizeof(r), &n) != RES_DATA &&
        to_card(wupa, sizeof(wupa), r, sizeof(r), &n) != RES_DATA) {
        return 0;
    }
    if (to_card(anticoll1, sizeof(anticoll1), r, sizeof(r), &n) != RES_DATA || n < 5) return 0;
    if ((r[0] ^ r[1] ^ r[2] ^ r[3]) != r[4]) return 0;  // check byte
    if (r[0] != 0x88) {
        memcpy(uid, r, 4);  // a 4-byte UID
        return 4;
    }
    // 0x88 means more follows: select this level, then read the second.
    uint8_t select[8] = { 0x93, 0x70, r[0], r[1], r[2], r[3], r[4], 0x28 };  // 0x28 = append CRC
    memcpy(uid, r + 1, 3);
    if (to_card(select, sizeof(select), r, sizeof(r), &n) != RES_DATA) return 0;
    if (to_card(anticoll2, sizeof(anticoll2), r, sizeof(r), &n) != RES_DATA || n < 5) return 0;
    if ((r[0] ^ r[1] ^ r[2] ^ r[3]) != r[4]) return 0;
    memcpy(uid + 3, r, 4);
    return 7;
}

static void post(ui_event_type_t type)
{
    const ui_event_t ev = { .type = type };
    xQueueSend(s_events, &ev, 0);
}

static void nfc_task(void *arg)
{
    int misses = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        uint8_t uid[NFC_UID_MAX] = { 0 };
        const int len = read_card(uid);
        if (len) {
            misses = 0;
            if (len != s_uid_len || memcmp(uid, s_uid, len) != 0) {
                const bool swapped = s_uid_len != 0;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                memcpy(s_uid, uid, sizeof(s_uid));
                s_uid_len = len;
                xSemaphoreGive(s_lock);
                char text[16];
                nfc_uid_text(text, sizeof(text));
                ESP_LOGI(TAG, "card %s in", text);
                if (swapped) post(UI_EV_CARD_OUT);
                post(UI_EV_CARD_IN);
            }
        } else if (s_uid_len && ++misses >= MISSES_GONE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_uid_len = 0;
            xSemaphoreGive(s_lock);
            misses = 0;
            ESP_LOGI(TAG, "card out");
            post(UI_EV_CARD_OUT);
        }
    }
}

esp_err_t nfc_init(QueueHandle_t events)
{
    s_events = events;
    s_lock = xSemaphoreCreateMutex();
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = 2000000,
        .mode = 0,
        .spics_io_num = -1,  // chip select is on the IO expander
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "SPI device");
    s_tx = heap_caps_malloc(BUF, MALLOC_CAP_DMA);
    s_rx = heap_caps_malloc(BUF, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_tx && s_rx && s_lock, ESP_ERR_NO_MEM, TAG, "buffers");
    if (!reader_start()) {
        ESP_LOGW(TAG, "the card reader did not answer; cards are disabled");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "card reader ready");
    xTaskCreatePinnedToCore(nfc_task, "nfc", 4096, NULL, 3, NULL, 1);
    return ESP_OK;
}

int nfc_uid(uint8_t uid[NFC_UID_MAX])
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int len = s_uid_len;
    memcpy(uid, s_uid, NFC_UID_MAX);
    xSemaphoreGive(s_lock);
    return len;
}

void nfc_uid_text(char *out, size_t len)
{
    uint8_t uid[NFC_UID_MAX];
    const int n = nfc_uid(uid);
    out[0] = 0;
    for (int i = 0; i < n && (size_t)(2 * i + 3) <= len; i++) sprintf(out + 2 * i, "%02X", uid[i]);
}
