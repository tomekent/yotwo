#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "pins.h"

static const char *TAG = "storage";

// The Yoto Gen 2 wires the card for SPI, on its own bus (not the display/NFC one).
static sdmmc_card_t *s_card;
static sdmmc_host_t s_host;
static sdspi_device_config_t s_slot;
static bool s_bus_up;
#define MOUNT_CFG(format) { .format_if_mount_failed = (format), .max_files = 8, .allocation_unit_size = 32 * 1024 }

/* The helper task for callers with PSRAM stacks (see storage.h). */

typedef struct {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} job_t;

static QueueHandle_t s_jobs;

static void worker(void *unused)
{
    for (;;) {
        job_t *j;
        xQueueReceive(s_jobs, &j, portMAX_DELAY);
        j->fn(j->arg);
        xSemaphoreGive(j->done);
    }
}

void storage_call(void (*fn)(void *), void *arg)
{
    if (!s_jobs || esp_ptr_in_dram((const void *)esp_cpu_get_sp())) {
        fn(arg);
        return;
    }
    StaticSemaphore_t buf;
    job_t job = { fn, arg, xSemaphoreCreateBinaryStatic(&buf) };
    job_t *p = &job;
    xQueueSend(s_jobs, &p, portMAX_DELAY);
    xSemaphoreTake(job.done, portMAX_DELAY);
}

esp_err_t storage_init(void)
{
    s_jobs = xQueueCreate(4, sizeof(job_t *));
    xTaskCreatePinnedToCore(worker, "sd", 5120, NULL, 3, NULL, 1);
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = MOUNT_CFG(false);  // never wipe a card the user put in
    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    s_host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    s_host.slot = SPI3_HOST;
    // Not SDSPI_DEFAULT_DMA: that is channel 1, which the display's bus has already taken.
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        s_slot = (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
        s_slot.gpio_cs = PIN_SD_CS;
        s_slot.host_id = SPI3_HOST;
        s_bus_up = true;
        // A card that was being written to when the player was reset (it keeps its power
        // through a reset) can take a moment to answer, or miss the first attempt: try a
        // few times before giving up on it. (2026-10-08: "sdmmc_card_init failed (0x107)"
        // after a reset in the middle of a download, and no card for that whole session.)
        for (int attempt = 1; attempt <= 5; attempt++) {
            err = esp_vfs_fat_sdspi_mount("/sdcard", &s_host, &s_slot, &mount_cfg, &s_card);
            if (err == ESP_OK) break;
            ESP_LOGW(TAG, "SD card did not answer (%s), attempt %d of 5", esp_err_to_name(err), attempt);
            s_card = NULL;
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no usable SD card (%s); caching and downloads disabled", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    storage_mkdirs(STORAGE_ROOT);
    uint64_t free_b, total_b;
    storage_space(&free_b, &total_b);
    ESP_LOGI(TAG, "SD card '%s' mounted: %llu MB free of %llu MB", s_card->cid.name, free_b >> 20, total_b >> 20);
    return ESP_OK;
}

bool storage_ready(void)
{
    return s_card != NULL;
}

void storage_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    *free_bytes = *total_bytes = 0;
    if (s_card) esp_vfs_fat_info("/sdcard", total_bytes, free_bytes);
}

// (From ABSpresso, kept for the SPI card too.) SDMMC DMA can't target PSRAM reliably (it
// corrupts data), so file data always passes through
// a small internal buffer. Large PSRAM buffers are copied in and out of it chunk by chunk.
#define BOUNCE 4096

static char *read_file_here(const char *path, size_t *len)
{
    if (len) *len = 0;
    if (!s_card) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM) : NULL;
    uint8_t *bounce = heap_caps_malloc(BOUNCE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    for (long off = 0; buf && bounce && off < n;) {
        size_t chunk = (n - off) < BOUNCE ? (n - off) : BOUNCE;
        if (fread(bounce, 1, chunk, f) != chunk) {
            free(buf);
            buf = NULL;
            break;
        }
        memcpy(buf + off, bounce, chunk);
        off += chunk;
    }
    if (!bounce) {
        free(buf);
        buf = NULL;
    }
    free(bounce);
    fclose(f);
    if (buf) {
        buf[n] = 0;
        if (len) *len = n;
    }
    return buf;
}

static esp_err_t write_file_here(const char *path, const void *data, size_t len)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    uint8_t *bounce = heap_caps_malloc(BOUNCE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    bool ok = bounce != NULL;
    for (size_t off = 0; ok && off < len;) {
        size_t chunk = (len - off) < BOUNCE ? (len - off) : BOUNCE;
        memcpy(bounce, (const uint8_t *)data + off, chunk);
        ok = fwrite(bounce, 1, chunk, f) == chunk;
        off += chunk;
    }
    free(bounce);
    ok &= fclose(f) == 0;
    if (ok) {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        ESP_LOGW(TAG, "writing %s failed (errno %d)", path, errno);
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t mkdirs_here(const char *path)
{
    char p[160];
    strlcpy(p, path, sizeof(p));
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0775);
            *s = '/';
        }
    }
    return (mkdir(p, 0775) == 0 || errno == EEXIST) ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    struct dirent *e;
    char child[320];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child)) continue;
        if (e->d_type == DT_DIR) storage_remove_tree(child);
        else unlink(child);
    }
    closedir(d);
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

/* The public calls, each run where its stack is internal. */

typedef struct { const char *path; size_t *len; char *out; } read_job_t;
static void read_job(void *arg) { read_job_t *j = arg; j->out = read_file_here(j->path, j->len); }

char *storage_read_file(const char *path, size_t *len)
{
    read_job_t j = { path, len, NULL };
    storage_call(read_job, &j);
    return j.out;
}

typedef struct { const char *path; const void *data; size_t len; esp_err_t err; } write_job_t;
static void write_job(void *arg) { write_job_t *j = arg; j->err = write_file_here(j->path, j->data, j->len); }

esp_err_t storage_write_file(const char *path, const void *data, size_t len)
{
    write_job_t j = { path, data, len, ESP_FAIL };
    storage_call(write_job, &j);
    return j.err;
}

typedef struct { const char *path; esp_err_t err; long size; } path_job_t;
static void mkdirs_job(void *arg) { path_job_t *j = arg; j->err = mkdirs_here(j->path); }

esp_err_t storage_mkdirs(const char *path)
{
    path_job_t j = { path, ESP_FAIL, 0 };
    storage_call(mkdirs_job, &j);
    return j.err;
}

static void size_job(void *arg)
{
    path_job_t *j = arg;
    struct stat sb;
    j->size = s_card && stat(j->path, &sb) == 0 ? (long)sb.st_size : -1;
}

long storage_file_size(const char *path)
{
    path_job_t j = { path, ESP_OK, -1 };
    storage_call(size_job, &j);
    return j.size;
}

/* Formatting, only ever on request (the web page, confirmed with a code on the display). */

static void format_job(void *arg)
{
    esp_err_t *err = arg;
    if (!s_bus_up) {
        *err = ESP_ERR_INVALID_STATE;
        return;
    }
    if (s_card) {
        // A card in use: make its file system afresh.
        ESP_LOGW(TAG, "formatting the SD card");
        esp_vfs_fat_mount_config_t cfg = MOUNT_CFG(false);
        *err = esp_vfs_fat_sdcard_format_cfg("/sdcard", s_card, &cfg);
    } else {
        // A card that could not be used as it was (blank, or in a format this player does
        // not read): mounting with leave to format does it.
        ESP_LOGW(TAG, "formatting an SD card that could not be read");
        const esp_vfs_fat_sdmmc_mount_config_t cfg = MOUNT_CFG(true);
        *err = esp_vfs_fat_sdspi_mount("/sdcard", &s_host, &s_slot, &cfg, &s_card);
        if (*err != ESP_OK) s_card = NULL;
    }
    if (*err == ESP_OK) {
        mkdirs_here(STORAGE_ROOT);
        ESP_LOGW(TAG, "SD card formatted");
    } else {
        ESP_LOGE(TAG, "formatting failed: %s", esp_err_to_name(*err));
    }
}

esp_err_t storage_format(void)
{
    esp_err_t err = ESP_FAIL;
    storage_call(format_job, &err);
    return err;
}
