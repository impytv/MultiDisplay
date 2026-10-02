#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "cal_client.h"
#include "http_util.h"
#include "yr_client.h"

static const char *TAG = "cal_client";

#define CAL_TIMEOUT_MS 30000
#define CAL_MAX_BYTES  (16 * 1024 * 1024) /* years of history stream through in pieces */

static bool feed(const char *data, size_t len, void *ctx)
{
    ical_feed(ctx, data, len);
    return true;
}

/* Calendar i's events out of the list, after a failed read. */
static void drop_feed(cal_list_t *l, uint8_t i)
{
    for (int k = 0; k < l->count;) {
        if (l->ev[k].feed == i) {
            l->ev[k] = l->ev[--l->count];
        } else {
            k++;
        }
    }
}

esp_err_t cal_fetch(const char *const *urls, int n, int64_t from, int64_t to, cal_list_t *out, uint8_t *failed)
{
    out->count = 0;
    *failed = 0;
    int read = 0;
    char *url = heap_caps_malloc(strlen("https://") + 512, MALLOC_CAP_SPIRAM);
    if (url == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < n; i++) {
        if (urls[i][0] == '\0') {
            continue;
        }
        /* webcal:// is https:// to a browser too. */
        if (strncmp(urls[i], "webcal://", 9) == 0) {
            snprintf(url, strlen("https://") + 512, "https://%s", urls[i] + 9);
        } else {
            snprintf(url, strlen("https://") + 512, "%s", urls[i]);
        }
        ical_parser_t *p = ical_begin(out, (uint8_t)i, from, to);
        if (p == NULL) {
            *failed |= (uint8_t)(1u << i);
            continue;
        }
        esp_err_t err = http_get_stream(url, yr_client_user_agent(), CAL_TIMEOUT_MS, CAL_MAX_BYTES, true, feed, p, TAG);
        const int events = ical_end(p);
        if (err != ESP_OK || events < 0) {
            if (err == ESP_OK) {
                ESP_LOGE(TAG, "Calendar %d isn't an iCal file", i + 1);
            }
            drop_feed(out, (uint8_t)i);
            *failed |= (uint8_t)(1u << i);
            continue;
        }
        ESP_LOGI(TAG, "Calendar %d: %d event(s) in the next days", i + 1, events);
        read++;
    }
    heap_caps_free(url);
    cal_sort(out);
    return *failed == 0 ? ESP_OK : read > 0 ? ESP_OK : ESP_FAIL;
}
