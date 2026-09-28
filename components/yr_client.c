#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "yr_client.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "http_util.h"

static const char *TAG = "yr_client";

#define YR_HTTP_TIMEOUT_MS   15000
#define YR_MAX_RESPONSE_LEN  (512 * 1024) /* safety cap against a runaway server */

/* Fetched with http_util's http_get_body, which sets no
 * crt_bundle_attach/cacert: esp-tls then configures MBEDTLS_SSL_VERIFY_NONE
 * (its documented behavior with no CA configured), skipping certificate
 * verification. Chosen deliberately - the HARICA/GEANT chain behind
 * api.met.no needs a 4096-bit RSA verify that doesn't reliably fit in this
 * board's internal RAM once LVGL+FreeType+WiFi have their share, and this is
 * a read-only fetch of public weather data, not a channel carrying secrets. */

/* Precipitation/symbol are reported for a period following the instant
 * (next_1_hours, falling back to next_6_hours/next_12_hours when the
 * 1-hour breakdown isn't available for that time step, e.g. further out
 * in the 9-day forecast). Amounts from a wider period are not rescaled -
 * they are still the best available estimate for "upcoming precipitation"
 * at that point. */
static void parse_period_fallback(cJSON *data, float *out_precip_mm, float *out_precip_min_mm,
                                  float *out_precip_max_mm, char *out_symbol, size_t symbol_len)
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
        /* MET's forecast uncertainty range for the same period - min/max, not
         * always present (e.g. missing this far out even when the point
         * estimate above is). Default to the point estimate (no bar behind
         * it worth drawing) unless both are actually reported. */
        cJSON *precip_min = cJSON_GetObjectItemCaseSensitive(details, "precipitation_amount_min");
        cJSON *precip_max = cJSON_GetObjectItemCaseSensitive(details, "precipitation_amount_max");
        if (cJSON_IsNumber(precip_min) && cJSON_IsNumber(precip_max)) {
            *out_precip_min_mm = (float)precip_min->valuedouble;
            *out_precip_max_mm = (float)precip_max->valuedouble;
        } else {
            *out_precip_min_mm = *out_precip_mm;
            *out_precip_max_mm = *out_precip_mm;
        }

        cJSON *summary = cJSON_GetObjectItemCaseSensitive(period, "summary");
        cJSON *symbol = cJSON_GetObjectItemCaseSensitive(summary, "symbol_code");
        if (cJSON_IsString(symbol)) {
            snprintf(out_symbol, symbol_len, "%s", symbol->valuestring);
        }

        return;
    }
}


/* Broken-down local time for an ISO8601 UTC timestamp, honouring the TZ the
 * app set (Europe/Oslo, so CET/CEST incl. DST). Returns false if unparseable. */
static bool iso_utc_to_local(const char *iso_utc, struct tm *out_local)
{
    int64_t epoch = iso8601_to_epoch(iso_utc);
    if (epoch <= 0) {
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
    json_use_psram();
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
        point->epoch_utc = iso8601_to_epoch(time_str);

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

        parse_period_fallback(data, &point->precipitation_mm, &point->precipitation_min_mm,
                              &point->precipitation_max_mm, point->symbol_code, sizeof(point->symbol_code));

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

/* The User-Agent MET asks for: the app and a contact (see the setup page). */
static char s_user_agent[96];

void yr_client_set_contact_email(const char *email)
{
    if (email == NULL || email[0] == '\0') {
        s_user_agent[0] = '\0';
    } else {
        snprintf(s_user_agent, sizeof(s_user_agent), "MultiDisplay/1.0 (%s)", email);
    }
}

const char *yr_client_user_agent(void)
{
    return s_user_agent[0] ? s_user_agent : CONFIG_EXAMPLE_YR_USER_AGENT;
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
    esp_err_t err = http_get_body(url, yr_client_user_agent(), YR_HTTP_TIMEOUT_MS, YR_MAX_RESPONSE_LEN, &body, TAG);
    if (err != ESP_OK) {
        return err;
    }

    bool ok = parse_forecast(body, out);
    free(body);
    return ok ? ESP_OK : ESP_FAIL;
}

static bool parse_nowcast(const char *json, yr_nowcast_t *out)
{
    json_use_psram();
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
        point->epoch_utc = iso8601_to_epoch(time_str);
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
    esp_err_t err = http_get_body(url, yr_client_user_agent(), YR_HTTP_TIMEOUT_MS, YR_MAX_RESPONSE_LEN, &body, TAG);
    if (err != ESP_OK) {
        return err;
    }

    bool ok = parse_nowcast(body, out);
    free(body);
    return ok ? ESP_OK : ESP_FAIL;
}
