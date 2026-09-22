#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "yr_client.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "yr_client";

#define YR_HTTP_TIMEOUT_MS   15000
#define YR_MAX_RESPONSE_LEN  (512 * 1024) /* safety cap against a runaway server */

/* cJSON's default allocator is plain malloc(), which ESP-IDF only routes to
 * PSRAM for allocations >= 1 KB (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) - most
 * cJSON parse-tree nodes are far smaller than that, so parsing a big response
 * (the "complete" locationforecast product is ~90 KB of JSON, several times
 * compact's) can otherwise burn through internal DRAM at the same time WiFi
 * needs its own, causing intermittent alloc failures there. Routing cJSON
 * straight to heap_caps_malloc(..., MALLOC_CAP_SPIRAM) sidesteps that size
 * threshold entirely. This is global to the cJSON library, hence the guard. */
static void *cjson_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
static void cjson_psram_free(void *ptr) { heap_caps_free(ptr); }

static void ensure_cjson_psram_hooks(void)
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc, .free_fn = cjson_psram_free };
    cJSON_InitHooks(&hooks);
}

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

/* Seconds since the Unix epoch for an ISO8601 UTC timestamp
 * ("YYYY-MM-DDTHH:MM:SS...", trailing zone ignored); -1 if unparseable.
 *
 * timegm() isn't available in newlib on ESP-IDF and mktime() would apply the
 * local offset to fields that are already UTC, so the epoch is computed with
 * the days-from-civil algorithm (Howard Hinnant) - pure arithmetic, no
 * dependency on the system clock being set. */
static int64_t iso_utc_to_epoch(const char *iso_utc)
{
    struct tm utc = { 0 };
    if (iso_utc == NULL ||
        sscanf(iso_utc, "%d-%d-%dT%d:%d:%d",
               &utc.tm_year, &utc.tm_mon, &utc.tm_mday,
               &utc.tm_hour, &utc.tm_min, &utc.tm_sec) != 6) {
        return -1;
    }

    int y = utc.tm_year;
    int m = utc.tm_mon;
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)utc.tm_mday - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (int64_t)days * 86400 + utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
}

/* Broken-down local time for an ISO8601 UTC timestamp, honouring the TZ the
 * app set (Europe/Oslo, so CET/CEST incl. DST). Returns false if unparseable. */
static bool iso_utc_to_local(const char *iso_utc, struct tm *out_local)
{
    int64_t epoch = iso_utc_to_epoch(iso_utc);
    if (epoch < 0) {
        return false;
    }
    time_t t = (time_t)epoch;
    localtime_r(&t, out_local);
    return true;
}

/* Fill "HH:MM" (Europe/Oslo local time) from an ISO8601 UTC timestamp. */
static void format_local_hm(const char *time_str, char *out, size_t out_len)
{
    struct tm local;
    if (iso_utc_to_local(time_str, &local)) {
        snprintf(out, out_len, "%02d:%02d", local.tm_hour, local.tm_min);
    } else {
        out[0] = '\0';
    }
}

/* Axis label: "HH:MM" in local time, plus a flag for the entry that lands on
 * local midnight (the start of a new day along the x-axis). */
static void extract_hour_minute(const char *time_str, char *out, size_t out_len, bool *is_first_of_day)
{
    struct tm local;
    if (!iso_utc_to_local(time_str, &local)) {
        out[0] = '\0';
        *is_first_of_day = false;
        return;
    }

    snprintf(out, out_len, "%02d:%02d", local.tm_hour, local.tm_min);
    *is_first_of_day = (local.tm_hour == 0);
}

static bool parse_forecast(const char *json, yr_forecast_t *out)
{
    ensure_cjson_psram_hooks();
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
        format_local_hm(updated_at->valuestring, out->updated_hour_minute,
                        sizeof(out->updated_hour_minute));
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
        if (n >= YR_FORECAST_BASE_POINTS) {
            break;
        }

        yr_forecast_point_t *point = &out->points[n];
        memset(point, 0, sizeof(*point));

        cJSON *time = cJSON_GetObjectItemCaseSensitive(entry, "time");
        const char *time_str = cJSON_IsString(time) ? time->valuestring : NULL;
        extract_hour_minute(time_str, point->hour_minute, sizeof(point->hour_minute),
                             &point->is_first_of_day);
        point->epoch_utc = iso_utc_to_epoch(time_str);

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
        cJSON *gust = cJSON_GetObjectItemCaseSensitive(instant_details, "wind_speed_of_gust");
        if (cJSON_IsNumber(gust)) {
            point->wind_speed_of_gust_ms = (float)gust->valuedouble;
        }
        cJSON *wind_dir = cJSON_GetObjectItemCaseSensitive(instant_details, "wind_from_direction");
        if (cJSON_IsNumber(wind_dir)) {
            point->wind_from_deg = (float)wind_dir->valuedouble;
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

/* GET url into a freshly heap_caps_malloc'd (PSRAM) NUL-terminated buffer;
 * caller frees *body on success. Returns ESP_OK only on HTTP 200 with a body.
 *
 * No crt_bundle_attach/cacert: esp-tls then configures MBEDTLS_SSL_VERIFY_NONE
 * (its documented behavior with no CA configured), skipping certificate
 * verification. Chosen deliberately - the HARICA/GEANT chain behind
 * api.met.no needs a 4096-bit RSA verify that doesn't reliably fit in this
 * board's internal RAM once LVGL+FreeType+WiFi have their share, and this is
 * a read-only fetch of public weather data, not a channel carrying secrets. */
static esp_err_t yr_http_get(const char *url, char **body)
{
    *body = NULL;

    yr_response_buf_t resp = { .buf = NULL, .len = 0 };
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
    if (resp.buf == NULL) {
        return ESP_FAIL;
    }

    *body = resp.buf;
    return ESP_OK;
}

esp_err_t yr_client_fetch_forecast(double lat, double lon, yr_forecast_t *out)
{
    memset(out, 0, sizeof(*out));

    /* "complete", not "compact": the wind chart wants wind_speed_of_gust,
     * which compact strips out. ~90 KB vs ~37 KB for a 9-day forecast -
     * well under YR_MAX_RESPONSE_LEN, and only the first YR_FORECAST_BASE_POINTS
     * entries are kept regardless. */
    char url[192];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/locationforecast/2.0/complete?lat=%.4f&lon=%.4f",
             lat, lon);

    char *body = NULL;
    esp_err_t err = yr_http_get(url, &body);
    if (err != ESP_OK) {
        return err;
    }

    bool ok = parse_forecast(body, out);
    free(body);
    return ok ? ESP_OK : ESP_FAIL;
}

static bool parse_nowcast(const char *json, yr_nowcast_t *out)
{
    ensure_cjson_psram_hooks();
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse nowcast JSON");
        return false;
    }

    bool ok = false;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(root, "properties");
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(properties, "meta");

    cJSON *radar = cJSON_GetObjectItemCaseSensitive(meta, "radar_coverage");
    out->radar_ok = (cJSON_IsString(radar) && strcmp(radar->valuestring, "ok") == 0);

    cJSON *updated_at = cJSON_GetObjectItemCaseSensitive(meta, "updated_at");
    if (cJSON_IsString(updated_at)) {
        format_local_hm(updated_at->valuestring, out->updated_hour_minute,
                        sizeof(out->updated_hour_minute));
    }

    cJSON *timeseries = cJSON_GetObjectItemCaseSensitive(properties, "timeseries");
    if (!cJSON_IsArray(timeseries) || cJSON_GetArraySize(timeseries) == 0) {
        ESP_LOGE(TAG, "No nowcast timeseries entries");
        goto done;
    }

    int n = 0;
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, timeseries)
    {
        if (n >= YR_NOWCAST_MAX_POINTS) {
            break;
        }

        yr_nowcast_point_t *point = &out->points[n];
        memset(point, 0, sizeof(*point));

        cJSON *time = cJSON_GetObjectItemCaseSensitive(entry, "time");
        const char *time_str = cJSON_IsString(time) ? time->valuestring : NULL;
        extract_hour_minute(time_str, point->hour_minute, sizeof(point->hour_minute),
                             &point->is_first_of_day);
        point->epoch_utc = iso_utc_to_epoch(time_str);
        if (point->epoch_utc < 0) {
            continue;
        }

        cJSON *data = cJSON_GetObjectItemCaseSensitive(entry, "data");
        cJSON *instant = cJSON_GetObjectItemCaseSensitive(data, "instant");
        cJSON *idetails = cJSON_GetObjectItemCaseSensitive(instant, "details");

        cJSON *rate = cJSON_GetObjectItemCaseSensitive(idetails, "precipitation_rate");
        if (!cJSON_IsNumber(rate)) {
            /* Every real step carries at least this; skip anything that doesn't. */
            continue;
        }
        point->precipitation_rate = (float)rate->valuedouble;

        cJSON *temp = cJSON_GetObjectItemCaseSensitive(idetails, "air_temperature");
        if (cJSON_IsNumber(temp)) {
            point->air_temperature_c = (float)temp->valuedouble;
            point->has_instant_details = true;

            cJSON *wind = cJSON_GetObjectItemCaseSensitive(idetails, "wind_speed");
            if (cJSON_IsNumber(wind)) {
                point->wind_speed_ms = (float)wind->valuedouble;
            }
            cJSON *gust = cJSON_GetObjectItemCaseSensitive(idetails, "wind_speed_of_gust");
            if (cJSON_IsNumber(gust)) {
                point->wind_speed_of_gust_ms = (float)gust->valuedouble;
            }
            cJSON *wind_dir = cJSON_GetObjectItemCaseSensitive(idetails, "wind_from_direction");
            if (cJSON_IsNumber(wind_dir)) {
                point->wind_from_deg = (float)wind_dir->valuedouble;
            }
        }

        cJSON *n1h = cJSON_GetObjectItemCaseSensitive(data, "next_1_hours");
        cJSON *summary = cJSON_GetObjectItemCaseSensitive(n1h, "summary");
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(summary, "symbol_code");
        if (cJSON_IsString(symbol)) {
            snprintf(point->symbol_code, sizeof(point->symbol_code), "%s", symbol->valuestring);
        }

        n++;
    }

    if (n == 0) {
        ESP_LOGE(TAG, "No usable nowcast steps");
        goto done;
    }

    out->point_count = n;
    out->valid = true;
    ok = true;

done:
    cJSON_Delete(root);
    return ok;
}

esp_err_t yr_client_fetch_nowcast(double lat, double lon, yr_nowcast_t *out)
{
    memset(out, 0, sizeof(*out));

    char url[192];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/nowcast/2.0/complete?lat=%.4f&lon=%.4f",
             lat, lon);

    char *body = NULL;
    esp_err_t err = yr_http_get(url, &body);
    if (err != ESP_OK) {
        return err;
    }

    bool ok = parse_nowcast(body, out);
    free(body);
    return ok ? ESP_OK : ESP_FAIL;
}
