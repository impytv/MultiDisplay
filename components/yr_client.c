#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "yr_client.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "yr_client";

#define YR_HTTP_TIMEOUT_MS   15000
#define YR_MAX_RESPONSE_LEN  (512 * 1024) /* safety cap against a runaway server */

typedef struct {
    char *buf;
    size_t len;
} yr_response_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    yr_response_buf_t *resp = (yr_response_buf_t *)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }

    if (resp->len + evt->data_len + 1 > YR_MAX_RESPONSE_LEN) {
        ESP_LOGE(TAG, "Response too large, aborting");
        return ESP_FAIL;
    }

    /* Keep this in PSRAM: mbedtls/esp_http_client already compete hard for
     * scarce internal DRAM while streaming the response, and this buffer
     * has no need to be internal. */
    char *new_buf = heap_caps_realloc(resp->buf, resp->len + evt->data_len + 1, MALLOC_CAP_SPIRAM);
    if (new_buf == NULL) {
        ESP_LOGE(TAG, "Out of memory growing response buffer to %u bytes",
                 (unsigned)(resp->len + evt->data_len + 1));
        return ESP_FAIL;
    }
    resp->buf = new_buf;
    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += evt->data_len;
    resp->buf[resp->len] = '\0';

    return ESP_OK;
}

/* Precipitation/symbol are reported for a period following the instant
 * (next_1_hours, falling back to next_6_hours/next_12_hours when the
 * 1-hour breakdown isn't available for that time step, e.g. further out
 * in the 9-day forecast). Amounts from a wider period are not rescaled -
 * they are still the best available estimate for "upcoming precipitation"
 * at that point. */
static void parse_period_fallback(cJSON *data, float *out_precip_mm, char *out_symbol, size_t symbol_len)
{
    static const char *periods[] = { "next_1_hours", "next_6_hours", "next_12_hours" };

    for (size_t i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
        cJSON *period = cJSON_GetObjectItemCaseSensitive(data, periods[i]);
        if (period == NULL) {
            continue;
        }

        cJSON *details = cJSON_GetObjectItemCaseSensitive(period, "details");
        cJSON *precip = cJSON_GetObjectItemCaseSensitive(details, "precipitation_amount");
        if (cJSON_IsNumber(precip)) {
            *out_precip_mm = (float)precip->valuedouble;
        }

        cJSON *summary = cJSON_GetObjectItemCaseSensitive(period, "summary");
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(summary, "symbol_code");
        if (cJSON_IsString(symbol)) {
            snprintf(out_symbol, symbol_len, "%s", symbol->valuestring);
        }

        return;
    }
}

/* time is ISO8601 "YYYY-MM-DDTHH:MM:SSZ"; pull "HH:MM" out directly. */
static void extract_hour_minute(const char *time_str, char *out, size_t out_len, bool *is_first_of_day)
{
    out[0] = '\0';
    *is_first_of_day = false;

    if (time_str == NULL || strlen(time_str) < 16) {
        return;
    }

    snprintf(out, out_len, "%.5s", time_str + 11);
    *is_first_of_day = (strncmp(time_str + 11, "00:", 3) == 0);
}

static bool parse_forecast(const char *json, yr_forecast_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    bool ok = false;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(root, "properties");

    cJSON *meta = cJSON_GetObjectItemCaseSensitive(properties, "meta");
    cJSON *updated_at = cJSON_GetObjectItemCaseSensitive(meta, "updated_at");
    if (cJSON_IsString(updated_at)) {
        snprintf(out->updated_time, sizeof(out->updated_time), "%s", updated_at->valuestring);
    }

    cJSON *timeseries = cJSON_GetObjectItemCaseSensitive(properties, "timeseries");
    if (!cJSON_IsArray(timeseries) || cJSON_GetArraySize(timeseries) == 0) {
        ESP_LOGE(TAG, "No timeseries entries in response");
        goto done;
    }

    int n = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, timeseries)
    {
        if (n >= YR_FORECAST_MAX_POINTS) {
            break;
        }

        yr_forecast_point_t *point = &out->points[n];
        memset(point, 0, sizeof(*point));

        cJSON *time = cJSON_GetObjectItemCaseSensitive(entry, "time");
        extract_hour_minute(cJSON_IsString(time) ? time->valuestring : NULL,
                             point->hour_minute, sizeof(point->hour_minute),
                             &point->is_first_of_day);

        cJSON *data = cJSON_GetObjectItemCaseSensitive(entry, "data");
        cJSON *instant = cJSON_GetObjectItemCaseSensitive(data, "instant");
        cJSON *instant_details = cJSON_GetObjectItemCaseSensitive(instant, "details");

        cJSON *temp = cJSON_GetObjectItemCaseSensitive(instant_details, "air_temperature");
        if (!cJSON_IsNumber(temp)) {
            /* No point in keeping a forecast entry with no temperature. */
            continue;
        }
        point->air_temperature_c = (float)temp->valuedouble;

        cJSON *wind = cJSON_GetObjectItemCaseSensitive(instant_details, "wind_speed");
        if (cJSON_IsNumber(wind)) {
            point->wind_speed_ms = (float)wind->valuedouble;
        }

        parse_period_fallback(data, &point->precipitation_mm, point->symbol_code, sizeof(point->symbol_code));

        n++;
    }

    if (n == 0) {
        ESP_LOGE(TAG, "No usable timeseries entries in response");
        goto done;
    }

    out->point_count = n;
    out->valid = true;
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

esp_err_t yr_client_fetch_forecast(double lat, double lon, yr_forecast_t *out)
{
    memset(out, 0, sizeof(*out));

    char url[192];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%.4f&lon=%.4f",
             lat, lon);

    yr_response_buf_t resp = { .buf = NULL, .len = 0 };

    /* No crt_bundle_attach/cacert: esp-tls then configures
     * MBEDTLS_SSL_VERIFY_NONE (its documented behavior with no CA
     * configured), skipping certificate verification. Chosen deliberately -
     * the HARICA/GEANT chain behind this host needs a 4096-bit RSA verify
     * that doesn't reliably fit in this board's internal RAM once
     * LVGL+FreeType+WiFi have their share, and this is a read-only fetch of
     * public weather data, not a channel carrying secrets. */
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = YR_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "User-Agent", CONFIG_EXAMPLE_YR_USER_AGENT);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status %d", status);
        free(resp.buf);
        return ESP_FAIL;
    }

    if (resp.buf == NULL || !parse_forecast(resp.buf, out)) {
        free(resp.buf);
        return ESP_FAIL;
    }

    free(resp.buf);
    return ESP_OK;
}
