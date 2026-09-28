#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "air_client.h"
#include "yr_client.h"

static const char *TAG = "air_client";

#define AIR_TIMEOUT_MS      15000
#define AIR_QUALITY_MAX     (512 * 1024) /* ~125 KB for two days */
#define AIR_POLLEN_MAX      (16 * 1024)

/* --------------------------------------------------------------------------
 * MET's reply is ~125 KB: some forty variables for every hour, of which
 * five are wanted. Rather than a cJSON tree of it all (tens of thousands of
 * nodes), each hour's object is scanned for just those.
 * ------------------------------------------------------------------------ */

/* The end of the string or object/array starting at p. */
static const char *skip_value(const char *p, const char *end)
{
    if (*p == '"') {
        for (p++; p < end; p++) {
            if (*p == '\\') {
                p++;
            } else if (*p == '"') {
                return p + 1;
            }
        }
        return end;
    }
    int depth = 0;
    for (; p < end; p++) {
        if (*p == '"') {
            p = skip_value(p, end) - 1;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0) {
                return p + 1;
            }
        }
    }
    return end;
}

/* Where the value of "key" starts, anywhere in [p, end), or NULL. */
static const char *find_key(const char *p, const char *end, const char *key)
{
    char pat[40];
    const int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    for (; p + n <= end; p++) {
        p = memchr(p, '"', (size_t)(end - p));
        if (p == NULL || p + n > end) {
            return NULL;
        }
        if (memcmp(p, pat, (size_t)n) == 0) {
            const char *v = p + n;
            while (v < end && (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t')) {
                v++;
            }
            if (v < end && *v == ':') {
                v++;
                while (v < end && (*v == ' ' || *v == '\n' || *v == '\r' || *v == '\t')) {
                    v++;
                }
                return v;
            }
        }
    }
    return NULL;
}

/* variables."name".value in one hour's object, or NAN. */
static float variable(const char *obj, const char *end, const char *name)
{
    const char *v = find_key(obj, end, name);
    if (v == NULL || *v != '{') {
        return NAN;
    }
    const char *vend = skip_value(v, end);
    const char *num = find_key(v, vend, "value");
    return num ? strtof(num, NULL) : NAN;
}

bool air_quality_parse(const char *json, size_t len, air_quality_t *out)
{
    static const char *const SUB[AIR_POLLUTANTS] = { "AQI_pm25", "AQI_pm10", "AQI_no2", "AQI_o3" };
    const char *end = json + len;
    memset(out, 0, sizeof(*out));
    const char *data = find_key(json, end, "data");
    const char *p = data ? find_key(data, end, "time") : NULL;
    if (p == NULL || *p != '[') {
        return false;
    }
    const char *arr_end = skip_value(p, end);
    int64_t prev = 0;
    for (p++; p < arr_end && out->count < AIR_HOURS;) {
        p = memchr(p, '{', (size_t)(arr_end - p));
        if (p == NULL) {
            break;
        }
        const char *obj_end = skip_value(p, arr_end);
        const char *from = find_key(p, obj_end, "from");
        const int64_t t = (from && *from == '"') ? iso8601_to_epoch(from + 1) : 0;
        const float aqi = variable(p, obj_end, "AQI");
        /* Hourly and in order; stop at a gap rather than misplace hours. */
        if (t == 0 || isnan(aqi) || (prev != 0 && t != prev + 3600)) {
            if (out->count > 0) {
                break;
            }
            p = obj_end;
            continue;
        }
        if (out->count == 0) {
            out->start = t;
        }
        out->aqi[out->count] = aqi;
        for (int s = 0; s < AIR_POLLUTANTS; s++) {
            const float v = variable(p, obj_end, SUB[s]);
            out->sub[s][out->count] = isnan(v) ? 0 : v;
        }
        out->count++;
        prev = t;
        p = obj_end;
    }
    out->valid = out->count > 0;
    return out->valid;
}

bool air_pollen_parse(const char *json, air_pollen_t *out)
{
    static const char *const KEYS[AIR_POLLEN_TYPES] = { "alder_pollen", "birch_pollen", "grass_pollen",
                                                        "mugwort_pollen" };
    memset(out, 0, sizeof(*out));
    json_use_psram();
    cJSON *root = cJSON_Parse(json);
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *times = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    const int n = cJSON_GetArraySize(times);
    if (!cJSON_IsArray(times) || n < 1) {
        cJSON_Delete(root);
        return false;
    }
    out->count = n < AIR_HOURS ? n : AIR_HOURS;
    out->start = (int64_t)cJSON_GetArrayItem(times, 0)->valuedouble;
    for (int k = 0; k < AIR_POLLEN_TYPES; k++) {
        const cJSON *vals = cJSON_GetObjectItemCaseSensitive(hourly, KEYS[k]);
        for (int i = 0; i < out->count; i++) {
            const cJSON *v = cJSON_GetArrayItem(vals, i);
            out->grains[k][i] = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.0f;
        }
    }
    cJSON_Delete(root);
    out->valid = true;
    return true;
}

esp_err_t air_quality_fetch(double lat, double lon, air_quality_t *out, http_cache_t *cache)
{
    char url[128];
    snprintf(url, sizeof(url), "https://api.met.no/weatherapi/airqualityforecast/0.1/?lat=%.4f&lon=%.4f", lat, lon);
    char *body = NULL;
    esp_err_t err = http_get_body_cached(url, yr_client_user_agent(), AIR_TIMEOUT_MS, AIR_QUALITY_MAX, &body, TAG,
                                         cache, false);
    if (err != ESP_OK) {
        return err;
    }
    const bool ok = air_quality_parse(body, strlen(body), out);
    free(body);
    if (!ok) {
        ESP_LOGE(TAG, "Couldn't read the air quality forecast");
        if (cache != NULL) {
            memset(cache, 0, sizeof(*cache));
        }
    }
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t air_pollen_fetch(double lat, double lon, air_pollen_t *out)
{
    char url[224];
    snprintf(url, sizeof(url),
             "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f"
             "&hourly=alder_pollen,birch_pollen,grass_pollen,mugwort_pollen&forecast_days=2&timeformat=unixtime",
             lat, lon);
    char *body = NULL;
    esp_err_t err = http_get_body(url, yr_client_user_agent(), AIR_TIMEOUT_MS, AIR_POLLEN_MAX, &body, TAG);
    if (err != ESP_OK) {
        return err;
    }
    const bool ok = air_pollen_parse(body, out);
    free(body);
    if (!ok) {
        ESP_LOGE(TAG, "Couldn't read the pollen forecast");
    }
    return ok ? ESP_OK : ESP_FAIL;
}

int air_hour_index(int64_t start, int count, int64_t now)
{
    if (count <= 0 || now < start) {
        return -1;
    }
    const int64_t i = (now - start) / 3600;
    return i < count ? (int)i : -1;
}

int air_aqi_level(float aqi)
{
    return aqi < 2.0f ? 0 : aqi < 3.0f ? 1 : aqi < 4.0f ? 2 : 3;
}

const char *air_aqi_level_name(int level)
{
    static const char *const N[] = { "Lite", "Moderat", "H\xC3\xB8y", "Sv\xC3\xA6rt h\xC3\xB8y" };
    return N[level < 0 ? 0 : level > 3 ? 3 : level];
}

uint32_t air_aqi_level_colour(int level)
{
    static const uint32_t C[] = { 0x3E9E4B, 0xE8B600, 0xD6312B, 0x8B2A8C };
    return C[level < 0 ? 0 : level > 3 ? 3 : level];
}

const char *air_pollutant_name(air_pollutant_t p)
{
    static const char *const N[AIR_POLLUTANTS] = { "svevest\xC3\xB8v (PM2,5)", "svevest\xC3\xB8v (PM10)",
                                                   "nitrogendioksid (NO2)", "ozon (O3)" };
    return N[p];
}

int air_pollen_level(air_pollen_type_t type, float grains)
{
    /* NAAF's limits per m3 of air: trees 1/10/100/1000, herbs 1/10/30/150. */
    static const float TREE[] = { 1, 10, 100, 1000 }, HERB[] = { 1, 10, 30, 150 };
    const float *lim = (type == AIR_ALDER || type == AIR_BIRCH) ? TREE : HERB;
    int level = 0;
    while (level < 4 && grains >= lim[level]) {
        level++;
    }
    return level;
}

const char *air_pollen_level_name(int level)
{
    static const char *const N[] = { "Ingen", "Beskjeden", "Moderat", "Kraftig", "Ekstrem" };
    return N[level < 0 ? 0 : level > 4 ? 4 : level];
}

uint32_t air_pollen_level_colour(int level)
{
    static const uint32_t C[] = { 0x9AA5AE, 0x3E9E4B, 0xE8B600, 0xE06A1B, 0x8B2A8C };
    return C[level < 0 ? 0 : level > 4 ? 4 : level];
}

const char *air_pollen_name(air_pollen_type_t type)
{
    static const char *const N[AIR_POLLEN_TYPES] = { "Or", "Bj\xC3\xB8rk", "Gress", "Burot" };
    return N[type];
}
