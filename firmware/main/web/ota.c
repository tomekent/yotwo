#include "ota.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "storage.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "ota";

#define TRIAL_US     (5 * 60 * 1000000LL)  // a new firmware has this long to be kept
#define PROVE_US     (60 * 1000000LL)      // ...and must have run at least this long
#define UI_FRESH_US  (3 * 1000000LL)       // the UI task must have drawn a frame this recently
#define CODE_US      (2 * 60 * 1000000LL)
#define CHECK_PREV_US (10 * 1000000LL)     // the other slot is checked this long after start-up
#define SETTLED_US   (2 * 60 * 1000000LL)  // running this long, a firmware is not crash-looping
#define CRASH_LIMIT  3

// Kept across restarts other than power-on (RTC memory that start-up does not clear): how
// many times running the firmware has ended in a crash before it settled.
#define CRASH_MAGIC  0x59325452u
static RTC_NOINIT_ATTR uint32_t s_crash_magic;
static RTC_NOINIT_ATTR uint32_t s_crashes;

static bool s_trial;
static esp_timer_handle_t s_deadline;
static char s_code[8];
static int64_t s_code_until;
static int s_wrong;

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static bool s_open;

// The other slot: checked once (reading and hashing 2 MB is not for every page refresh).
static enum { PREV_UNCHECKED, PREV_NONE, PREV_OK } s_prev;
static char s_prev_version[32];

static bool on_trial_now(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
}

// Fired by its own timer, so that a firmware stuck in its main loop still goes back.
static void deadline(void *arg)
{
    if (!on_trial_now()) return;
    ESP_LOGE(TAG, "the new firmware has not proved itself in five minutes: going back to the previous one");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

// Whether `slot` holds firmware the player can go back to: complete and intact (its own
// checksum and hash), this project, and not one that has already failed a trial. A slot
// written by cable has no trial record, which is fine.
static bool usable(const esp_partition_t *slot, esp_app_desc_t *desc)
{
    if (!slot || esp_ota_get_partition_description(slot, desc) != ESP_OK) return false;
    if (strcmp(desc->project_name, esp_app_get_description()->project_name) != 0) return false;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(slot, &state) == ESP_OK &&
        (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)) {
        return false;
    }
    const esp_partition_pos_t pos = { .offset = slot->address, .size = slot->size };
    esp_image_metadata_t meta;
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK;
}

typedef struct { char *why; size_t why_len; esp_err_t err; } back_job_t;

static void back_job(void *arg)
{
    back_job_t *j = arg;
    if (s_open) {
        snprintf(j->why, j->why_len, "an update is being written");
        j->err = ESP_ERR_INVALID_STATE;
        return;
    }
    if (on_trial_now()) {
        // Turning down the firmware on trial: the bootloader starts the one before it.
        j->err = esp_ota_check_rollback_is_possible() ? esp_ota_mark_app_invalid_rollback() : ESP_ERR_NOT_FOUND;
    } else {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t desc;
        // Set to start, on trial (the bootloader's record for it is "new").
        j->err = usable(other, &desc) ? esp_ota_set_boot_partition(other) : ESP_ERR_NOT_FOUND;
        if (j->err == ESP_OK) ESP_LOGW(TAG, "going back to firmware %s in slot %s", desc.version, other->label);
    }
    if (j->err == ESP_ERR_NOT_FOUND) snprintf(j->why, j->why_len, "there is no earlier firmware to go back to");
    else if (j->err != ESP_OK) snprintf(j->why, j->why_len, "could not set it to start (%s)", esp_err_to_name(j->err));
}

esp_err_t ota_go_back(char *why, size_t why_len)
{
    why[0] = 0;
    back_job_t j = { why, why_len, ESP_FAIL };
    storage_call(back_job, &j);
    return j.err;
}

void ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    s_trial = on_trial_now();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "firmware %s (built %s %s) in slot %s%s", app->version, app->date, app->time,
             running ? running->label : "?", s_trial ? ": on trial after an update" : "");

    // A firmware on trial that crashes is the bootloader's to undo (any restart during the
    // trial does it). A kept one that keeps crashing soon after starting is ours.
    const esp_reset_reason_t why = esp_reset_reason();
    if (s_crash_magic != CRASH_MAGIC || why == ESP_RST_POWERON) {
        s_crash_magic = CRASH_MAGIC;
        s_crashes = 0;
    }
    if (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT) {
        s_crashes++;
        ESP_LOGW(TAG, "the last run ended in a crash (reason %d); %u in a row", (int)why, (unsigned)s_crashes);
    } else {
        s_crashes = 0;  // switched on, restarted on purpose, or a brown-out: not the firmware
    }
    if (!s_trial && s_crashes >= CRASH_LIMIT) {
        s_crashes = 0;
        char reason[64];
        if (ota_go_back(reason, sizeof(reason)) == ESP_OK) {
            ESP_LOGE(TAG, "%d crashes in a row soon after starting: going back to the previous firmware", CRASH_LIMIT);
            esp_restart();
        }
        ESP_LOGE(TAG, "%d crashes in a row, but cannot go back: %s", CRASH_LIMIT, reason);
    }

    if (s_trial) {
        const esp_timer_create_args_t args = { .callback = deadline, .name = "ota trial" };
        if (esp_timer_create(&args, &s_deadline) == ESP_OK) esp_timer_start_once(s_deadline, TRIAL_US);
    }
}

bool ota_busy(void)
{
    return s_open;
}

const char *ota_state(void)
{
    return s_trial ? "trial" : "";
}

bool ota_previous(char *version, size_t len)
{
    if (s_prev != PREV_OK || s_open) return false;
    strlcpy(version, s_prev_version, len);
    return true;
}

static void check_prev_job(void *arg)
{
    esp_app_desc_t desc;
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (usable(other, &desc)) {
        strlcpy(s_prev_version, desc.version, sizeof(s_prev_version));
        s_prev = PREV_OK;
        ESP_LOGI(TAG, "can go back to firmware %s in slot %s", desc.version, other->label);
    } else {
        s_prev = PREV_NONE;
        ESP_LOGI(TAG, "no earlier firmware to go back to");
    }
}

void ota_watch(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_crashes && now > SETTLED_US) s_crashes = 0;
    if (s_prev == PREV_UNCHECKED && now > CHECK_PREV_US && !s_open) storage_call(check_prev_job, NULL);
    if (!s_trial || now < PROVE_US) return;
    // Kept when it can take another update and the player is usable: the web page has
    // answered a request (the browser's, or the player's own to itself) and the UI task is
    // still drawing. Not the media server: an outage there says nothing about the firmware.
    // (The first trial on the player was passed 4.7 s after start, the moment the page came
    // up, which would have kept a build that fell over at 10.)
    const int64_t ui_age = now - ui_alive_us();
    if (ui_age > UI_FRESH_US || !web_check()) return;
    if (s_deadline) esp_timer_stop(s_deadline);
    if (!on_trial_now()) {  // the deadline got there first
        s_trial = false;
        return;
    }
    s_trial = false;
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "the new firmware has run %d s, answered on its web page and is drawing: kept", (int)(now / 1000000));
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
    s_prev = PREV_NONE;  // being overwritten
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
