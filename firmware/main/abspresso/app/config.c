#include "config.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "config";

static app_config_t *s_cfg;  // in PSRAM: the tokens are large
static SemaphoreHandle_t s_lock;

// Flash (NVS) work from tasks with PSRAM stacks is handed to this worker, which has an internal
// stack. It's started at boot, while internal RAM is plentiful: creating a task per save could
// fail later (with the setup portal up, there isn't a 4 KB block left) and lose the save.
#define FLASH_STACK 3072

typedef struct {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} flash_job_t;

static QueueHandle_t s_jobs;

static void flash_worker(void *unused)
{
    for (;;) {
        flash_job_t *j;
        xQueueReceive(s_jobs, &j, portMAX_DELAY);
        j->fn(j->arg);
        xSemaphoreGive(j->done);
    }
}

void flash_safe(void (*fn)(void *), void *arg)
{
    if (esp_ptr_in_dram((const void *)esp_cpu_get_sp())) {
        fn(arg);
        return;
    }
    StaticSemaphore_t buf;
    flash_job_t job = {fn, arg, xSemaphoreCreateBinaryStatic(&buf)};
    flash_job_t *p = &job;
    xQueueSend(s_jobs, &p, portMAX_DELAY);
    xSemaphoreTake(job.done, portMAX_DELAY);
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = 0;
}

void config_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(4, sizeof(flash_job_t *));
    xTaskCreatePinnedToCore(flash_worker, "flash", FLASH_STACK, NULL, 4, NULL, 0);
    s_cfg = heap_caps_calloc(1, sizeof(*s_cfg), MALLOC_CAP_SPIRAM);
    s_cfg->skip_back_s = 30;
    s_cfg->skip_fwd_s = 30;
    s_cfg->rotate180 = true;

    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", s_cfg->wifi_ssid, sizeof(s_cfg->wifi_ssid));
        get_str(h, "pass", s_cfg->wifi_pass, sizeof(s_cfg->wifi_pass));
        get_str(h, "server", s_cfg->server, sizeof(s_cfg->server));
        get_str(h, "user", s_cfg->username, sizeof(s_cfg->username));
        get_str(h, "access", s_cfg->access, sizeof(s_cfg->access));
        get_str(h, "refresh", s_cfg->refresh, sizeof(s_cfg->refresh));
        uint8_t v;
        if (nvs_get_u8(h, "skipb", &v) == ESP_OK) s_cfg->skip_back_s = v;
        if (nvs_get_u8(h, "skipf", &v) == ESP_OK) s_cfg->skip_fwd_s = v;
        nvs_close(h);
    }
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "rot180", &v) == ESP_OK) s_cfg->rotate180 = v;
        nvs_close(h);
    }

    ESP_LOGI(TAG, "wifi '%s', server %s, %s", s_cfg->wifi_ssid, s_cfg->server,
             s_cfg->refresh[0] ? "signed in" : (s_cfg->access[0] ? "API key" : "no credentials"));
}

const app_config_t *config_get(void)
{
    return s_cfg;
}

static void write_all(void *unused)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't save settings: %s", esp_err_to_name(err));
        return;
    }
    const esp_err_t errs[] = {
        nvs_set_str(h, "ssid", s_cfg->wifi_ssid),
        nvs_set_str(h, "pass", s_cfg->wifi_pass),
        nvs_set_str(h, "server", s_cfg->server),
        nvs_set_str(h, "user", s_cfg->username),
        nvs_set_str(h, "access", s_cfg->access),
        nvs_set_str(h, "refresh", s_cfg->refresh),
        nvs_set_u8(h, "skipb", s_cfg->skip_back_s),
        nvs_set_u8(h, "skipf", s_cfg->skip_fwd_s),
        nvs_commit(h),
    };
    for (int i = 0; i < sizeof(errs) / sizeof(errs[0]); i++) {
        if (errs[i] != ESP_OK) ESP_LOGE(TAG, "saving settings failed (step %d): %s", i, esp_err_to_name(errs[i]));
    }
    nvs_close(h);
    if (nvs_open("ui", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "rot180", s_cfg->rotate180);
    nvs_commit(h);
    nvs_close(h);
}

void config_save(const app_config_t *cfg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (cfg != s_cfg) *s_cfg = *cfg;
    flash_safe(write_all, NULL);
    xSemaphoreGive(s_lock);
}

void config_set_tokens(const char *access, const char *refresh)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg->access, access, sizeof(s_cfg->access));
    if (refresh) strlcpy(s_cfg->refresh, refresh, sizeof(s_cfg->refresh));
    flash_safe(write_all, NULL);
    xSemaphoreGive(s_lock);
}

void config_set_skip(int back_s, int fwd_s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg->skip_back_s = back_s;
    s_cfg->skip_fwd_s = fwd_s;
    flash_safe(write_all, NULL);
    xSemaphoreGive(s_lock);
}

void config_set_rotate(bool rotate180)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg->rotate180 = rotate180;
    flash_safe(write_all, NULL);
    xSemaphoreGive(s_lock);
}

bool config_complete(void)
{
    return s_cfg->wifi_ssid[0] && s_cfg->server[0] && s_cfg->access[0];
}
