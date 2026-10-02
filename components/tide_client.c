#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

#include "http_util.h"
#include "tide_client.h"
#include "yr_client.h"

static const char *TAG = "tide_client";

#define TIDE_TIMEOUT_MS  15000
#define TIDE_SERIES_MAX  (160 * 1024) /* ~45 KB for four series of 48 hours */
#define TIDE_TABLE_MAX   (16 * 1024)
#define TIDE_URL         "https://vannstand.kartverket.no/tideapi.php?tide_request=locationdata" \
                         "&lat=%.4f&lon=%.4f&refcode=CD&lang=nb&tzone=0&fromtime=%s&totime=%s"

/* --------------------------------------------------------------------------
 * The replies are small XML documents of one shape:
 *   <tide><locationdata><location name=".." .../>
 *     <data type="prediction" ...><waterlevel value="88.6" time="..." flag="low"/>...</data>
 *   </locationdata></tide>
 * or <nodata info="..."/> in place of the location and data. Read by
 * looking for those elements and their attributes.
 * ------------------------------------------------------------------------ */

/* The value of attribute `name` in the element starting at `el` (up to its
 * '>'), copied into dst; false if it isn't there. */
static bool attr(const char *el, const char *name, char *dst, size_t len)
{
    const char *end = strchr(el, '>');
    char pat[24];
    const int n = snprintf(pat, sizeof(pat), " %s=\"", name);
    for (const char *p = el; end != NULL && (p = strstr(p, pat)) != NULL && p < end; p++) {
        const char *v = p + n, *q = strchr(v, '"');
        if (q == NULL || q > end) {
            return false;
        }
        const size_t k = (size_t)(q - v) < len - 1 ? (size_t)(q - v) : len - 1;
        memcpy(dst, v, k);
        dst[k] = '\0';
        return true;
    }
    return false;
}

/* Common to both replies: whether it is one, no data, and the station. */
static bool parse_head(const char *xml, tide_t *out)
{
    if (xml == NULL || strstr(xml, "<tide") == NULL) {
        return false;
    }
    out->valid = true;
    out->no_data = strstr(xml, "<nodata") != NULL;
    const char *loc = strstr(xml, "<location ");
    if (loc == NULL || !attr(loc, "name", out->station, sizeof(out->station))) {
        out->station[0] = '\0';
    }
    return true;
}

bool tide_parse_series(const char *xml, tide_t *out)
{
    for (int i = 0; i < TIDE_POINTS; i++) {
        out->prediction[i] = out->forecast[i] = out->observation[i] = NAN;
    }
    if (!parse_head(xml, out)) {
        return false;
    }
    for (const char *d = strstr(xml, "<data "); d != NULL; d = strstr(d + 1, "<data ")) {
        char type[24];
        if (!attr(d, "type", type, sizeof(type))) {
            continue;
        }
        float *series = strcmp(type, "prediction") == 0  ? out->prediction
                        : strcmp(type, "forecast") == 0  ? out->forecast
                        : strcmp(type, "observation") == 0 ? out->observation
                                                           : NULL;
        if (series == NULL) {
            continue; /* the weather effect: forecast minus prediction */
        }
        const char *end = strstr(d, "</data>");
        for (const char *w = strstr(d, "<waterlevel "); w != NULL && (end == NULL || w < end);
             w = strstr(w + 1, "<waterlevel ")) {
            char val[16], when[32];
            if (!attr(w, "value", val, sizeof(val)) || !attr(w, "time", when, sizeof(when))) {
                continue;
            }
            const int64_t t = iso8601_to_epoch(when);
            const int64_t k = (t - out->start) / TIDE_STEP_S;
            if (t != 0 && t >= out->start && (t - out->start) % TIDE_STEP_S == 0 && k < out->count) {
                series[k] = strtof(val, NULL);
            }
        }
    }
    return true;
}

bool tide_parse_extremes(const char *xml, tide_t *out)
{
    out->extreme_count = 0;
    if (!parse_head(xml, out)) {
        return false;
    }
    for (const char *w = strstr(xml, "<waterlevel "); w != NULL && out->extreme_count < TIDE_EXTREMES;
         w = strstr(w + 1, "<waterlevel ")) {
        char val[16], when[32], flag[8];
        if (!attr(w, "value", val, sizeof(val)) || !attr(w, "time", when, sizeof(when)) ||
            !attr(w, "flag", flag, sizeof(flag)) || (strcmp(flag, "high") != 0 && strcmp(flag, "low") != 0)) {
            continue;
        }
        tide_extreme_t *e = &out->extremes[out->extreme_count];
        e->time = iso8601_to_epoch(when);
        e->cm = strtof(val, NULL);
        e->high = flag[0] == 'h';
        if (e->time != 0) {
            out->extreme_count++;
        }
    }
    return true;
}

float tide_at(const tide_t *t, const float *series, int64_t when)
{
    if (t->count < 2 || when < t->start) {
        return NAN;
    }
    const int64_t off = when - t->start;
    const int k = (int)(off / TIDE_STEP_S);
    if (k < t->count && off % TIDE_STEP_S == 0) {
        return series[k];
    }
    if (k >= t->count - 1) {
        return NAN;
    }
    const float f = (float)(off % TIDE_STEP_S) / TIDE_STEP_S;
    return series[k] + (series[k + 1] - series[k]) * f; /* NAN if either is */
}

static void utc_text(int64_t t, char *dst, size_t len)
{
    const time_t tt = (time_t)t;
    struct tm u;
    gmtime_r(&tt, &u);
    strftime(dst, len, "%Y-%m-%dT%H:%MZ", &u);
}

static esp_err_t get(const char *type, double lat, double lon, int64_t from, int64_t to, size_t max, char **body)
{
    char a[24], b[24], url[256];
    utc_text(from, a, sizeof(a));
    utc_text(to, b, sizeof(b));
    int n = snprintf(url, sizeof(url), TIDE_URL "&datatype=%s", lat, lon, a, b, type);
    if (strcmp(type, "ALL") == 0) {
        snprintf(url + n, sizeof(url) - n, "&interval=%d", TIDE_STEP_S / 60);
    }
    return http_get_body(url, yr_client_user_agent(), TIDE_TIMEOUT_MS, max, body, TAG);
}

esp_err_t tide_fetch(double lat, double lon, int64_t now, tide_t *out)
{
    memset(out, 0, sizeof(*out));
    out->start = (now - 6 * 3600) / TIDE_STEP_S * TIDE_STEP_S;
    out->count = TIDE_POINTS;
    const int64_t to = out->start + (int64_t)(TIDE_POINTS - 1) * TIDE_STEP_S;
    char *body = NULL;
    esp_err_t err = get("ALL", lat, lon, out->start, to, TIDE_SERIES_MAX, &body);
    if (err != ESP_OK) {
        return err;
    }
    bool ok = tide_parse_series(body, out);
    free(body);
    if (ok && !out->no_data) {
        /* High and low tide over a little more, so the next ones are known
         * near the end too. */
        err = get("TAB", lat, lon, now - 6 * 3600, now + 54 * 3600, TIDE_TABLE_MAX, &body);
        if (err != ESP_OK) {
            return err;
        }
        ok = tide_parse_extremes(body, out);
        free(body);
    }
    if (!ok) {
        ESP_LOGE(TAG, "Couldn't read the tide reply");
        return ESP_FAIL;
    }
    return ESP_OK;
}
