#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "met_alerts_client.h"
#include "yr_client.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "http_util.h"

static const char *TAG = "met_alerts_client";

#define MET_ALERTS_HTTP_TIMEOUT_MS  15000
#define MET_ALERTS_MAX_RESPONSE_LEN (128 * 1024) /* safety cap against a runaway server */

static met_alert_color_t parse_color(const char *s)
{
    if (s != NULL) {
        if (strcasecmp(s, "Red") == 0) {
            return MET_ALERT_RED;
        }
        if (strcasecmp(s, "Orange") == 0) {
            return MET_ALERT_ORANGE;
        }
    }
    return MET_ALERT_YELLOW; /* the mildest colour is also MetAlerts' fallback */
}

/* The response is a GeoJSON FeatureCollection; an empty "features" array is a
 * valid, successful result meaning no alert covers this point, not an
 * error. */
bool met_alerts_parse(const char *json, met_alerts_t *out)
{
    memset(out, 0, sizeof(*out));
    json_use_psram();
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    bool ok = false;
    cJSON *features = cJSON_GetObjectItemCaseSensitive(root, "features");
    if (!cJSON_IsArray(features)) {
        ESP_LOGE(TAG, "No \"features\" array in response");
        goto done;
    }

    int n = 0;
    cJSON *feature = NULL;
    cJSON_ArrayForEach(feature, features)
    {
        if (n >= MET_ALERTS_MAX) {
            break;
        }

        cJSON *props = cJSON_GetObjectItemCaseSensitive(feature, "properties");
        if (props == NULL) {
            continue;
        }

        met_alert_t *a = &out->alerts[n];
        memset(a, 0, sizeof(*a));

        cJSON *name = cJSON_GetObjectItemCaseSensitive(props, "eventAwarenessName");
        if (cJSON_IsString(name)) {
            snprintf(a->event_name, sizeof(a->event_name), "%s", name->valuestring);
        }

        cJSON *area = cJSON_GetObjectItemCaseSensitive(props, "area");
        if (cJSON_IsString(area)) {
            snprintf(a->area, sizeof(a->area), "%s", area->valuestring);
        }

        cJSON *color = cJSON_GetObjectItemCaseSensitive(props, "riskMatrixColor");
        a->color = parse_color(cJSON_IsString(color) ? color->valuestring : NULL);

        /* "when": {"interval": ["<start>", "<end>"]} */
        cJSON *interval = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(feature, "when"),
                                                           "interval");
        cJSON *from = cJSON_GetArrayItem(interval, 0), *to = cJSON_GetArrayItem(interval, 1);
        a->start = cJSON_IsString(from) ? iso8601_to_epoch(from->valuestring) : 0;
        a->end = cJSON_IsString(to) ? iso8601_to_epoch(to->valuestring) : 0;

        n++;
    }

    out->count = n;
    out->valid = true;
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

esp_err_t met_alerts_client_fetch(double lat, double lon, met_alerts_t *out, http_cache_t *cache)
{

    char url[160];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/metalerts/2.0/current.json?lat=%.4f&lon=%.4f",
             lat, lon);

    char *body = NULL;
    esp_err_t err = http_get_body_cached(url, yr_client_user_agent(), MET_ALERTS_HTTP_TIMEOUT_MS,
                                         MET_ALERTS_MAX_RESPONSE_LEN, &body, TAG, cache, false);
    if (err != ESP_OK) {
        return err;
    }

    bool ok = met_alerts_parse(body, out);
    free(body);
    if (!ok && cache != NULL) {
        memset(cache, 0, sizeof(*cache));
    }
    return ok ? ESP_OK : ESP_FAIL;
}
