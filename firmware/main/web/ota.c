#include "ota.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "ota";

#define TRIAL_US     (5 * 60 * 1000000LL)  // a new firmware has this long to serve the web page
#define CODE_US      (2 * 60 * 1000000LL)
#define PROVE_US     (60 * 1000000LL)      // ...and must have run this long without falling over

static bool s_trial;
static char s_code[8];
static int64_t s_code_until;
static int s_wrong;

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static bool s_open;

void ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);
    s_trial = state == ESP_OTA_IMG_PENDING_VERIFY;
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "firmware %s (built %s %s) in slot %s%s", app->version, app->date, app->time,
             running ? running->label : "?", s_trial ? ": on trial after an update" : "");
}

bool ota_busy(void)
{
    return s_open;
}

const char *ota_state(void)
{
    return s_trial ? "trial" : "";
}

void ota_watch(bool web_up)
{
    if (!s_trial) return;
    if (web_up && esp_timer_get_time() > PROVE_US) {
        // It has run for a minute and can take another update, so it need not be kept from
        // one: good enough. (The first trial on the player was passed 4.7 s after start,
        // the moment the page came up, which would have kept a build that fell over at 10.)
        s_trial = false;
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "the new firmware is serving its web page: kept");
    } else if (esp_timer_get_time() > TRIAL_US) {
        ESP_LOGE(TAG, "the new firmware has not served its web page in five minutes: going back to the previous one");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void ota_new_code(void)
{
    snprintf(s_code, sizeof(s_code), "%04lu", (unsigned long)(esp_random() % 10000UL));
    s_code_until = esp_timer_get_time() + CODE_US;
    s_wrong = 0;
    ui_show_word(s_code, 120);
    ESP_LOGI(TAG, "update code on the display");
}

bool ota_code_ok(const char *code)
{
    if (!s_code[0] || esp_timer_get_time() > s_code_until) return false;
    if (code && strcmp(code, s_code) == 0) {
        s_code[0] = 0;  // one upload
        return true;
    }
    if (++s_wrong >= 5) s_code[0] = 0;  // no guessing: a new code has to be asked for
    return false;
}

/* The flash calls, each made on a stack in internal memory (flash work must not run on a
   PSRAM stack; the storage helper's task serves). */

typedef struct { size_t size; const void *data; size_t len; char *why; size_t why_len; esp_err_t err; } job_t;

static void begin_job(void *arg)
{
    job_t *j = arg;
    if (s_open) {
        esp_ota_abort(s_handle);
        s_open = false;
    }
    s_target = esp_ota_get_next_update_partition(NULL);
    if (!s_target) {
        ESP_LOGE(TAG, "no second firmware slot: the flash layout predates over-the-air updates");
        j->err = ESP_ERR_NOT_FOUND;
        return;
    }
    if (j->size > s_target->size) {
        j->err = ESP_ERR_INVALID_SIZE;
        return;
    }
    // Erased as it is written, so starting does not hold the flash for ten seconds.
    j->err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    s_open = j->err == ESP_OK;
    if (s_open) ESP_LOGI(TAG, "update of %u bytes into slot %s", (unsigned)j->size, s_target->label);
}

static void write_job(void *arg)
{
    job_t *j = arg;
    j->err = s_open ? esp_ota_write(s_handle, j->data, j->len) : ESP_ERR_INVALID_STATE;
}

static void end_job(void *arg)
{
    job_t *j = arg;
    if (!s_open) {
        j->err = ESP_ERR_INVALID_STATE;
        snprintf(j->why, j->why_len, "no update was in progress");
        return;
    }
    s_open = false;
    j->err = esp_ota_end(s_handle);  // checks the image's own checksum and hash
    if (j->err != ESP_OK) {
        snprintf(j->why, j->why_len, "the file is not a complete firmware image (%s)", esp_err_to_name(j->err));
        return;
    }
    // Only this player's firmware: the name built into the image must match ours.
    esp_app_desc_t desc;
    j->err = esp_ota_get_partition_description(s_target, &desc);
    if (j->err == ESP_OK && strcmp(desc.project_name, esp_app_get_description()->project_name) != 0) {
        snprintf(j->why, j->why_len, "that is firmware for \"%s\", not for this player", desc.project_name);
        j->err = ESP_ERR_INVALID_VERSION;
        return;
    }
    if (j->err == ESP_OK) j->err = esp_ota_set_boot_partition(s_target);
    if (j->err != ESP_OK) {
        snprintf(j->why, j->why_len, "could not make it the firmware to start (%s)", esp_err_to_name(j->err));
        return;
    }
    ESP_LOGI(TAG, "firmware %s (built %s %s) accepted: it starts at the next restart", desc.version, desc.date, desc.time);
}

static void abort_job(void *arg)
{
    if (s_open) esp_ota_abort(s_handle);
    s_open = false;
}

esp_err_t ota_begin(size_t size)
{
    job_t j = { .size = size, .err = ESP_FAIL };
    storage_call(begin_job, &j);
    return j.err;
}

esp_err_t ota_write(const void *data, size_t len)
{
    job_t j = { .data = data, .len = len, .err = ESP_FAIL };
    storage_call(write_job, &j);
    return j.err;
}

esp_err_t ota_end(char *why, size_t why_len)
{
    why[0] = 0;
    job_t j = { .why = why, .why_len = why_len, .err = ESP_FAIL };
    storage_call(end_job, &j);
    return j.err;
}

void ota_abort(void)
{
    job_t j = { 0 };
    storage_call(abort_job, &j);
}
