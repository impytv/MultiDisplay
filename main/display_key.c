#include "display_key.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "key";

/* The reports' and the sharing's tasks: not both making the key at once. */
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init = portMUX_INITIALIZER_UNLOCKED;

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static void lock(void)
{
    taskENTER_CRITICAL(&s_init);
    static StaticSemaphore_t buf;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&buf);
    }
    taskEXIT_CRITICAL(&s_init);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

/* The key from NVS, made and kept the first time (under the lock). Kept in
 * the namespace the error reports have used since 1.33, so a display keeps
 * the key the site already knows. */
static bool key_load(mbedtls_pk_context *pk)
{
    nvs_handle_t h;
    if (nvs_open("feil", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t der[160];
    size_t len = sizeof(der);
    bool ok = nvs_get_blob(h, "key", der, &len) == ESP_OK && mbedtls_pk_parse_key(pk, der, len, NULL, 0, rng, NULL) == 0;
    if (!ok) {
        mbedtls_pk_free(pk);
        mbedtls_pk_init(pk);
        int n = -1;
        if (mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
            mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), rng, NULL) == 0) {
            n = mbedtls_pk_write_key_der(pk, der, sizeof(der)); /* at the end of der */
        }
        ok = n > 0 && nvs_set_blob(h, "key", der + sizeof(der) - n, (size_t)n) == ESP_OK && nvs_commit(h) == ESP_OK;
        ESP_LOGI(TAG, "New key for the display: %s", ok ? "kept" : "failed");
    }
    nvs_close(h);
    return ok;
}

/* The public half (DER) at the end of spki: its start, and its length in
 * *len. NULL if it doesn't fit. */
static const uint8_t *public_der(mbedtls_pk_context *pk, uint8_t *spki, size_t cap, size_t *len)
{
    const int n = mbedtls_pk_write_pubkey_der(pk, spki, cap);
    if (n <= 0) {
        return NULL;
    }
    *len = (size_t)n;
    return spki + cap - n;
}

bool display_key_sign(const uint8_t sha[32], char *key_b64, size_t key_len, char *sig_b64, size_t sig_len)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    uint8_t spki[128], sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t n = 0, der_len = 0, out = 0;
    lock();
    bool ok = key_load(&pk);
    const uint8_t *der = ok ? public_der(&pk, spki, sizeof(spki), &der_len) : NULL;
    ok = der != NULL &&
         mbedtls_base64_encode((unsigned char *)key_b64, key_len, &out, der, der_len) == 0 &&
         mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, sha, 32, sig, sizeof(sig), &n, rng, NULL) == 0 &&
         mbedtls_base64_encode((unsigned char *)sig_b64, sig_len, &out, sig, n) == 0;
    xSemaphoreGive(s_lock);
    mbedtls_pk_free(&pk);
    return ok;
}

bool display_key_id(char out[17])
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    uint8_t spki[128], sha[32];
    size_t der_len = 0;
    lock();
    const uint8_t *der = key_load(&pk) ? public_der(&pk, spki, sizeof(spki), &der_len) : NULL;
    xSemaphoreGive(s_lock);
    mbedtls_pk_free(&pk);
    if (der == NULL) {
        out[0] = '\0';
        return false;
    }
    mbedtls_sha256(der, der_len, sha, 0);
    for (int i = 0; i < 8; i++) {
        snprintf(out + 2 * i, 3, "%02x", sha[i]);
    }
    return true;
}
