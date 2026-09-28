#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "civil_time.h"
#include "http_util.h"

/* A failure sooner than this is a dead connection, not a slow server. */
#define HTTP_QUICK_FAIL_US (3 * 1000 * 1000)

void http_buf_reset(http_buf_t *b)
{
    b->len = 0;
    if (b->buf != NULL) {
        b->buf[0] = '\0';
    }
}

void http_buf_free(http_buf_t *b)
{
    heap_caps_free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

esp_err_t http_buf_append(http_buf_t *b, const void *data, size_t len, const char *tag)
{
    const size_t need = b->len + len + 1;
    if (need > b->max) {
        ESP_LOGE(tag, "Response over %u bytes, aborting", (unsigned)b->max);
        return ESP_FAIL;
    }
    if (need > b->cap) {
        /* Doubling: a realloc per chunk would copy the body over and over. */
        size_t cap = b->cap ? b->cap * 2 : 16 * 1024;
        while (cap < need) {
            cap *= 2;
        }
        char *nb = heap_caps_realloc(b->buf, cap, MALLOC_CAP_SPIRAM);
        if (nb == NULL) {
            ESP_LOGE(tag, "Out of memory growing response buffer to %u", (unsigned)cap);
            return ESP_FAIL;
        }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
    b->buf[b->len] = '\0';
    return ESP_OK;
}

bool http_retry_worthwhile(bool reused, int64_t started_us)
{
    return reused && esp_timer_get_time() - started_us < HTTP_QUICK_FAIL_US;
}

static void *json_malloc(size_t sz)
{
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
}

static void json_free(void *ptr)
{
    heap_caps_free(ptr);
}

void json_use_psram(void)
{
    static bool done;
    if (!done) {
        done = true;
        cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = json_free };
        cJSON_InitHooks(&hooks);
    }
}

int64_t iso8601_to_epoch(const char *s)
{
    int y, mo, d, h, mi, se, n = 0;
    if (s == NULL || sscanf(s, "%d-%d-%dT%d:%d:%d%n", &y, &mo, &d, &h, &mi, &se, &n) != 6) {
        return 0;
    }
    const char *z = s + n;
    if (*z == '.') { /* fractional seconds */
        z++;
        while (isdigit((unsigned char)*z)) {
            z++;
        }
    }
    int off = 0;
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        sscanf(z + 1, "%d:%d", &oh, &om);
        off = (oh * 60 + om) * 60 * (*z == '-' ? -1 : 1);
    }
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se - off;
}

static esp_err_t get_body_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    http_buf_t *b = evt->user_data;
    return http_buf_append(b, evt->data, evt->data_len, "http_util");
}

esp_err_t http_get_body(const char *url, const char *user_agent, int timeout_ms, size_t max, char **body,
                        const char *tag)
{
    *body = NULL;
    http_buf_t b = { .max = max };
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = get_body_handler,
        .user_data = &b,
        .timeout_ms = timeout_ms,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "User-Agent", user_agent);
    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(tag, "HTTP request failed: %s", esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGE(tag, "Unexpected HTTP status %d", status);
        err = ESP_FAIL;
    } else if (b.buf == NULL) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        http_buf_free(&b);
        return err;
    }
    *body = b.buf;
    return ESP_OK;
}
