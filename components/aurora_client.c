#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "aurora_client.h"
#include "yr_client.h"

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "aurora_client";

#define AURORA_HTTP_TIMEOUT_MS  15000
#define AURORA_MAX_RESPONSE_LEN (96 * 1024) /* about 18 KB */

/* {"shortIntervals":[{"start":"2026-10-06T20:00:00+02:00","end":...,
 *  "kpIndex":2,"auroraValue":0.22,"sunlight":{"id":"night"},
 *  "cloudCover":{"value":94,...}}, ...]} */
bool aurora_parse(const char *json, aurora_t *out)
{
    memset(out, 0, sizeof(*out));
    json_use_psram();
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "shortIntervals");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, list)
    {
        if (out->count >= AURORA_MAX_HOURS) {
            break;
        }
        const cJSON *start = cJSON_GetObjectItemCaseSensitive(it, "start");
        const cJSON *end = cJSON_GetObjectItemCaseSensitive(it, "end");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(it, "auroraValue");
        const cJSON *kp = cJSON_GetObjectItemCaseSensitive(it, "kpIndex");
        const cJSON *sun = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(it, "sunlight"), "id");
        const cJSON *cloud =
            cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(it, "cloudCover"), "value");
        if (!cJSON_IsString(start) || !cJSON_IsString(end) || !cJSON_IsNumber(value)) {
            continue;
        }
        aurora_hour_t *h = &out->hours[out->count];
        h->start = iso8601_to_epoch(start->valuestring);
        h->end = iso8601_to_epoch(end->valuestring);
        if (h->start == 0 || h->end <= h->start) {
            continue;
        }
        h->value = (float)value->valuedouble;
        h->kp = cJSON_IsNumber(kp) && kp->valuedouble >= 0 && kp->valuedouble <= 9 ? (uint8_t)kp->valuedouble : 0;
        /* Unknown cloud counts as overcast, unknown light as day. */
        h->cloud = cJSON_IsNumber(cloud) && cloud->valuedouble >= 0 && cloud->valuedouble <= 100
                       ? (uint8_t)(cloud->valuedouble + 0.5)
                       : 100;
        h->dark = cJSON_IsString(sun) && strcmp(sun->valuestring, "day") != 0;
        out->count++;
    }
    out->valid = cJSON_IsArray(list);
    cJSON_Delete(root);
    if (!out->valid) {
        ESP_LOGE(TAG, "No \"shortIntervals\" in response");
    }
    return out->valid;
}

esp_err_t aurora_client_fetch(double lat, double lon, aurora_t *out, http_cache_t *cache)
{
    char url[128];
    snprintf(url, sizeof(url), "https://www.yr.no/api/v0/locations/%.4f,%.4f/auroraforecast", lat, lon);
    char *body = NULL;
    esp_err_t err = http_get_body_cached(url, yr_client_user_agent(), AURORA_HTTP_TIMEOUT_MS,
                                         AURORA_MAX_RESPONSE_LEN, &body, TAG, cache, false);
    if (err != ESP_OK) {
        return err;
    }
    const bool ok = aurora_parse(body, out);
    free(body);
    if (!ok && cache != NULL) {
        memset(cache, 0, sizeof(*cache));
    }
    return ok ? ESP_OK : ESP_FAIL;
}
