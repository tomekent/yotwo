#include "credential.h"

#include <string.h>
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "credential";

#define NAMESPACE "cred"
#define KEY       "pw"
#define MAX_LEN   127

typedef struct { uint8_t iv[16]; uint8_t data[MAX_LEN + 1]; size_t len; bool ok; } job_t;

static void make_key(uint8_t key[32])
{
    uint8_t seed[32] = "yotwo credential v1";
    esp_efuse_mac_get_default(seed + 24);
    mbedtls_sha256(seed, sizeof(seed), key, 0);
}

static void crypt(const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t key[32], counter[16], stream[16];
    size_t offset = 0;
    make_key(key);
    memcpy(counter, iv, 16);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 256);
    mbedtls_aes_crypt_ctr(&aes, len, &offset, counter, stream, in, out);
    mbedtls_aes_free(&aes);
    memset(key, 0, sizeof(key));
}

// These run where flash access is allowed (see flash_safe()).
static void write_job(void *arg)
{
    job_t *j = arg;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (j->len) {
        uint8_t blob[16 + MAX_LEN];
        memcpy(blob, j->iv, 16);
        memcpy(blob + 16, j->data, j->len);
        j->ok = nvs_set_blob(h, KEY, blob, 16 + j->len) == ESP_OK;
    } else {
        nvs_erase_key(h, KEY);
        j->ok = true;
    }
    nvs_commit(h);
    nvs_close(h);
}

static void read_job(void *arg)
{
    job_t *j = arg;
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t blob[16 + MAX_LEN];
    size_t n = sizeof(blob);
    if (nvs_get_blob(h, KEY, blob, &n) == ESP_OK && n > 16) {
        memcpy(j->iv, blob, 16);
        j->len = n - 16;
        memcpy(j->data, blob + 16, j->len);
        j->ok = true;
    }
    nvs_close(h);
}

void credential_save(const char *password)
{
    job_t j = { 0 };
    j.len = strlen(password);
    if (j.len == 0 || j.len > MAX_LEN) {
        credential_clear();
        return;
    }
    esp_fill_random(j.iv, sizeof(j.iv));
    crypt(j.iv, (const uint8_t *)password, j.data, j.len);
    flash_safe(write_job, &j);
    ESP_LOGI(TAG, "password %s", j.ok ? "kept for signing in again" : "could not be saved");
}

void credential_clear(void)
{
    job_t j = { 0 };
    flash_safe(write_job, &j);
}

bool credential_present(void)
{
    job_t j = { 0 };
    flash_safe(read_job, &j);
    return j.ok;
}

bool credential_load(char *out, size_t len)
{
    job_t j = { 0 };
    flash_safe(read_job, &j);
    if (!j.ok || j.len >= len) return false;
    crypt(j.iv, j.data, (uint8_t *)out, j.len);
    out[j.len] = 0;
    return true;
}
