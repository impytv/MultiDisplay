/* Host-test implementations of the ESP-IDF calls the tested files make: an
 * in-memory NVS, a clock, and an HTTP client that never connects. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "nvs.h"

int g_test_verbose;

const char *esp_err_to_name(esp_err_t err)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "err %d", err);
    return buf;
}

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    (void)config;
    return NULL;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) { return ESP_FAIL; }
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url) { return ESP_FAIL; }
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *d, int n) { return ESP_FAIL; }
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) { return ESP_FAIL; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return 0; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { return ESP_OK; }

/* NVS: one namespace's keys in a list, which is all app_config needs. */
typedef struct entry {
    char key[16];
    void *val;
    size_t len;
    struct entry *next;
} entry_t;
static entry_t *s_nvs;

void test_nvs_clear(void)
{
    while (s_nvs) {
        entry_t *n = s_nvs->next;
        free(s_nvs->val);
        free(s_nvs);
        s_nvs = n;
    }
}

static entry_t *find(const char *key)
{
    for (entry_t *e = s_nvs; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

static esp_err_t put(const char *key, const void *v, size_t len)
{
    entry_t *e = find(key);
    if (e == NULL) {
        e = calloc(1, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->next = s_nvs;
        s_nvs = e;
    }
    free(e->val);
    e->val = malloc(len);
    memcpy(e->val, v, len);
    e->len = len;
    return ESP_OK;
}

static esp_err_t get(const char *key, void *out, size_t *len, size_t want)
{
    entry_t *e = find(key);
    if (e == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (len == NULL) { /* a fixed-size integer */
        if (e->len != want) {
            return ESP_FAIL;
        }
        memcpy(out, e->val, want);
        return ESP_OK;
    }
    if (out == NULL) {
        *len = e->len;
        return ESP_OK;
    }
    if (*len < e->len) {
        return ESP_FAIL;
    }
    memcpy(out, e->val, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { *h = 1; return ESP_OK; }
void nvs_close(nvs_handle_t h) {}
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { test_nvs_clear(); return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    for (entry_t **p = &s_nvs; *p; p = &(*p)->next) {
        if (strcmp((*p)->key, key) == 0) {
            entry_t *e = *p;
            *p = e->next;
            free(e->val);
            free(e);
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len) { return get(k, out, len, 0); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len) { return get(k, out, len, 0); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *out) { return get(k, out, NULL, 1); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *out) { return get(k, out, NULL, 2); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return put(k, v, strlen(v) + 1); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { return put(k, v, len); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { return put(k, &v, 1); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t v) { return put(k, &v, 2); }
