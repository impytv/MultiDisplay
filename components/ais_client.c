#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ais_client.h"
#include "civil_time.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ais_client";

#define AIS_TOKEN_URL         "https://id.barentswatch.no/connect/token"
#define AIS_LATEST_URL        "https://live.ais.barentswatch.no/live/v1/latest/combined"
#define AIS_HTTP_TIMEOUT_MS   15000
/* ~250 bytes per ship in the Simple model, a few times that in the Full one;
 * a busy fjord is a few hundred ships. */
#define AIS_MAX_RESPONSE_LEN  (256 * 1024)
#define AIS_TOKEN_MAX         4096
#define AIS_TOKEN_MARGIN_S    60
/* Ships silent for longer than this are left out (moored ones report every
 * few minutes). */
#define AIS_SINCE_S           (15 * 60)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static resp_buf_t s_resp;
static esp_http_client_handle_t s_client;
static char *s_token;           /* PSRAM; "" when none */
static TickType_t s_token_expiry;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (s_resp.len + evt->data_len + 1 > AIS_MAX_RESPONSE_LEN) {
        ESP_LOGE(TAG, "Response too large, aborting");
        return ESP_FAIL;
    }
    if (s_resp.len + evt->data_len + 1 > s_resp.cap) {
        size_t new_cap = s_resp.cap ? s_resp.cap * 2 : 16 * 1024;
        while (new_cap < s_resp.len + evt->data_len + 1) {
            new_cap *= 2;
        }
        char *nb = heap_caps_realloc(s_resp.buf, new_cap, MALLOC_CAP_SPIRAM);
        if (nb == NULL) {
            ESP_LOGE(TAG, "Out of memory growing response buffer to %u", (unsigned)new_cap);
            return ESP_FAIL;
        }
        s_resp.buf = nb;
        s_resp.cap = new_cap;
    }
    memcpy(s_resp.buf + s_resp.len, evt->data, evt->data_len);
    s_resp.len += evt->data_len;
    s_resp.buf[s_resp.len] = '\0';
    return ESP_OK;
}

static void resp_reset(void)
{
    s_resp.len = 0;
    if (s_resp.buf != NULL) {
        s_resp.buf[0] = '\0';
    }
}

void ais_client_close(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    free(s_resp.buf);
    s_resp = (resp_buf_t){ 0 };
}

const char *ais_category_label(uint8_t category)
{
    switch (category) {
    case AIS_CAT_CARGO:     return "Last";
    case AIS_CAT_TANKER:    return "Tank";
    case AIS_CAT_PASSENGER: return "Pass";
    case AIS_CAT_FISHING:   return "Fiske";
    case AIS_CAT_LEISURE:   return "Fritid";
    case AIS_CAT_TUG:       return "Slep";
    default:                return "Annet";
    }
}

static uint8_t category_of(int ship_type)
{
    if (ship_type == 30) return AIS_CAT_FISHING;
    if (ship_type == 31 || ship_type == 32 || ship_type == 52) return AIS_CAT_TUG;
    if (ship_type == 36 || ship_type == 37) return AIS_CAT_LEISURE;
    if (ship_type >= 60 && ship_type <= 69) return AIS_CAT_PASSENGER;
    if (ship_type >= 70 && ship_type <= 79) return AIS_CAT_CARGO;
    if (ship_type >= 80 && ship_type <= 89) return AIS_CAT_TANKER;
    return AIS_CAT_OTHER;
}

/* --------------------------------------------------------------------------
 * Minimal JSON scanning - same approach and reasons as adsb_client (cJSON
 * would put hundreds of small nodes in internal DRAM).
 * ------------------------------------------------------------------------ */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *skip_string(const char *p, const char *end)
{
    p++;
    while (p < end && *p != '"') {
        p += (*p == '\\' && p + 1 < end) ? 2 : 1;
    }
    return p < end ? p + 1 : end;
}

static const char *skip_container(const char *p, const char *end)
{
    int depth = 0;
    while (p < end) {
        char c = *p;
        if (c == '"') {
            p = skip_string(p, end);
            continue;
        }
        if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (--depth == 0) {
                return p + 1;
            }
        }
        p++;
    }
    return end;
}

static const char *skip_value(const char *p, const char *end)
{
    if (p >= end) {
        return end;
    }
    if (*p == '"') {
        return skip_string(p, end);
    }
    if (*p == '{' || *p == '[') {
        return skip_container(p, end);
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    return p;
}

static double to_double(const char *p, const char *end)
{
    char tmp[32];
    size_t n = (size_t)(end - p);
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    return strtod(tmp, NULL);
}

static bool is_number_start(char c)
{
    return c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9');
}

/* "2024-05-01T12:34:56[.fff][Z|+hh:mm]" -> epoch seconds, or 0. */
static time_t parse_iso8601(const char *s, size_t n)
{
    char tmp[40];
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    int y, mo, d, h, mi, se;
    if (sscanf(tmp, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) {
        return 0;
    }
    int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    const char *tz = tmp + 19;
    while (*tz == '.' || (*tz >= '0' && *tz <= '9')) {
        tz++;
    }
    int oh, om;
    if ((*tz == '+' || *tz == '-') && sscanf(tz + 1, "%d:%d", &oh, &om) == 2) {
        int off = oh * 3600 + om * 60;
        t -= (*tz == '+') ? off : -off;
    }
    return (time_t)t;
}

typedef struct {
    bool has_lat, has_lon, has_cog, has_heading, has_sog;
    double lat, lon;
    float sog, cog, heading;
    float length;         /* metres, 0 if not reported */
    int ship_type, nav_status;
    uint32_t mmsi;
    time_t msgtime;
    char name[sizeof(((ais_ship_t *)0)->name)];
} raw_ship_t;

static void parse_ship(const char *p, const char *end, raw_ship_t *r)
{
    memset(r, 0, sizeof(*r));
    r->nav_status = 15; /* "not defined" */
    p++; /* '{' */
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == '}') {
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '"') {
            break;
        }
        const char *key = p + 1;
        const char *key_end = skip_string(p, end) - 1;
        size_t klen = (size_t)(key_end - key);
        p = skip_ws(key_end + 1, end);
        if (p >= end || *p != ':') {
            break;
        }
        p = skip_ws(p + 1, end);
        const char *val = p;
        const char *val_end = skip_value(val, end);
        p = val_end;

        bool is_str = (val < val_end && *val == '"');
        const char *sv = val + 1;
        size_t slen = (val_end - val >= 2) ? (size_t)(val_end - val - 2) : 0;
        bool is_num = (val < val_end && is_number_start(*val));

#define KEY_IS(s) (klen == sizeof(s) - 1 && memcmp(key, s, klen) == 0)
        if (KEY_IS("latitude") && is_num) {
            r->lat = to_double(val, val_end);
            r->has_lat = true;
        } else if (KEY_IS("longitude") && is_num) {
            r->lon = to_double(val, val_end);
            r->has_lon = true;
        } else if (KEY_IS("speedOverGround") && is_num) {
            r->sog = (float)to_double(val, val_end);
            r->has_sog = r->sog >= 0.0f && r->sog < 102.2f; /* 102.3 = not available */
        } else if (KEY_IS("courseOverGround") && is_num) {
            r->cog = (float)to_double(val, val_end);
            r->has_cog = r->cog >= 0.0f && r->cog < 360.0f; /* 360 = not available */
        } else if (KEY_IS("trueHeading") && is_num) {
            r->heading = (float)to_double(val, val_end);
            r->has_heading = r->heading >= 0.0f && r->heading < 360.0f; /* 511 = not available */
        } else if (KEY_IS("shipLength") && is_num) {
            r->length = (float)to_double(val, val_end);
        } else if (KEY_IS("shipType") && is_num) {
            r->ship_type = (int)to_double(val, val_end);
        } else if (KEY_IS("navigationalStatus") && is_num) {
            r->nav_status = (int)to_double(val, val_end);
        } else if (KEY_IS("mmsi") && is_num) {
            r->mmsi = (uint32_t)to_double(val, val_end);
        } else if (KEY_IS("msgtime") && is_str) {
            r->msgtime = parse_iso8601(sv, slen);
        } else if (KEY_IS("name") && is_str) {
            size_t n = slen < sizeof(r->name) - 1 ? slen : sizeof(r->name) - 1;
            /* AIS pads names with spaces or '@'. */
            while (n > 0 && (sv[n - 1] == ' ' || sv[n - 1] == '@')) {
                n--;
            }
            memcpy(r->name, sv, n);
            r->name[n] = '\0';
        }
#undef KEY_IS
    }
}

static bool parse_response(const char *json, size_t len, double lat0, double lon0,
                           float radius_km, uint16_t min_len_m, ais_result_t *out)
{
    const char *end = json + len;
    const char *p = skip_ws(json, end);
    if (p >= end || *p != '[') {
        return false;
    }
    p++;

    const time_t now = time(NULL);
    const double km_per_deg_lat = 110.574;
    const double km_per_deg_lon = 111.320 * cos(lat0 * M_PI / 180.0);

    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end || *p == ']') {
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '{') {
            break;
        }
        const char *obj_end = skip_container(p, end);
        raw_ship_t r;
        parse_ship(p, obj_end, &r);
        p = obj_end;

        if (!r.has_lat || !r.has_lon) {
            continue;
        }
        if (min_len_m > 0 && r.length < (float)min_len_m) {
            continue; /* too short, or length not reported */
        }
        float x_km = (float)((r.lon - lon0) * km_per_deg_lon);
        float y_km = (float)((r.lat - lat0) * km_per_deg_lat);
        float dist = sqrtf(x_km * x_km + y_km * y_km);
        if (dist > radius_km) {
            continue; /* in the query square's corners */
        }
        out->total++;

        ais_ship_t s = { 0 };
        if (r.name[0]) {
            snprintf(s.name, sizeof(s.name), "%s", r.name);
        } else {
            snprintf(s.name, sizeof(s.name), "%lu", (unsigned long)r.mmsi);
        }
        s.mmsi = r.mmsi;
        s.dist_km = dist;
        float brg = atan2f(x_km, y_km) * 180.0f / (float)M_PI;
        s.bearing_deg = brg < 0.0f ? brg + 360.0f : brg;
        s.sog_kn = r.has_sog ? r.sog : 0.0f;
        s.cog_deg = r.has_cog ? r.cog : (r.has_heading ? r.heading : 0.0f);
        s.heading_deg = r.has_heading ? r.heading : s.cog_deg;
        s.age_s = (r.msgtime > 0 && now > r.msgtime) ? (float)(now - r.msgtime) : 0.0f;
        s.category = category_of(r.ship_type);
        s.moored = r.nav_status == 1 || r.nav_status == 5 || s.sog_kn < 0.5f;

        int at = out->count;
        if (out->count == AIS_MAX_SHIPS) {
            if (s.dist_km >= out->ship[out->count - 1].dist_km) {
                continue;
            }
            at = out->count - 1;
        } else {
            out->count++;
        }
        while (at > 0 && out->ship[at - 1].dist_km > s.dist_km) {
            out->ship[at] = out->ship[at - 1];
            at--;
        }
        out->ship[at] = s;
    }
    return true;
}

/* --------------------------------------------------------------------------
 * OAuth token (client credentials)
 * ------------------------------------------------------------------------ */

/* Append `src` form-urlencoded to dst; returns the new end, or NULL if full. */
static char *form_encode(char *dst, const char *dst_end, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~';
        if (dst + (plain ? 1 : 3) >= dst_end) {
            return NULL;
        }
        if (plain) {
            *dst++ = (char)c;
        } else {
            *dst++ = '%';
            *dst++ = hex[c >> 4];
            *dst++ = hex[c & 0xF];
        }
    }
    *dst = '\0';
    return dst;
}

/* Find "key":"string" / "key":number in a small flat JSON object. */
static bool json_find(const char *json, const char *key, const char **val, size_t *vlen)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (k == NULL) {
        return false;
    }
    const char *end = json + strlen(json);
    const char *p = skip_ws(k + strlen(pat), end);
    if (p >= end || *p != ':') {
        return false;
    }
    p = skip_ws(p + 1, end);
    const char *v_end = skip_value(p, end);
    if (p < v_end && *p == '"') {
        *val = p + 1;
        *vlen = (size_t)(v_end - p - 2);
    } else {
        *val = p;
        *vlen = (size_t)(v_end - p);
    }
    return true;
}

/* ESP_OK, ESP_ERR_INVALID_STATE (credentials rejected) or ESP_FAIL. */
static esp_err_t fetch_token(const char *client_id, const char *client_secret)
{
    if (s_token == NULL) {
        s_token = heap_caps_calloc(1, AIS_TOKEN_MAX, MALLOC_CAP_SPIRAM);
        if (s_token == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_token[0] = '\0';

    char *body = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    char *end = body + 1024;
    char *p = body + snprintf(body, 1024, "grant_type=client_credentials&scope=ais&client_id=");
    p = form_encode(p, end, client_id);
    if (p != NULL && p + 16 < end) {
        p += snprintf(p, end - p, "&client_secret=");
        p = form_encode(p, end, client_secret);
    }
    if (p == NULL) {
        free(body);
        return ESP_ERR_INVALID_ARG;
    }

    resp_reset();
    esp_http_client_config_t cfg = {
        .url = AIS_TOKEN_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = AIS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        free(body);
        return ESP_FAIL;
    }
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_header(c, "User-Agent", CONFIG_EXAMPLE_YR_USER_AGENT);
    esp_http_client_set_post_field(c, body, (int)(p - body));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(body);

    if (err != ESP_OK || s_resp.buf == NULL) {
        ESP_LOGW(TAG, "token request failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    if (status == 400 || status == 401) {
        ESP_LOGE(TAG, "token request rejected (%d): %.200s", status, s_resp.buf);
        return ESP_ERR_INVALID_STATE;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "token request status %d", status);
        return ESP_FAIL;
    }

    const char *v;
    size_t vlen;
    if (!json_find(s_resp.buf, "access_token", &v, &vlen) || vlen == 0 || vlen >= AIS_TOKEN_MAX) {
        ESP_LOGW(TAG, "no access_token in token response");
        return ESP_FAIL;
    }
    memcpy(s_token, v, vlen);
    s_token[vlen] = '\0';
    if (vlen + 64 > AIS_TOKEN_MAX + 512) {
        ESP_LOGW(TAG, "access token is %u bytes, may not fit the request buffer", (unsigned)vlen);
    }

    int expires_s = 3600;
    if (json_find(s_resp.buf, "expires_in", &v, &vlen)) {
        expires_s = (int)to_double(v, v + vlen);
    }
    if (expires_s < 2 * AIS_TOKEN_MARGIN_S) {
        expires_s = 2 * AIS_TOKEN_MARGIN_S;
    }
    s_token_expiry = xTaskGetTickCount() + pdMS_TO_TICKS((expires_s - AIS_TOKEN_MARGIN_S) * 1000);
    ESP_LOGI(TAG, "got access token, valid %d s", expires_s);
    return ESP_OK;
}

static bool token_valid(void)
{
    return s_token != NULL && s_token[0] != '\0' &&
           (int32_t)(s_token_expiry - xTaskGetTickCount()) > 0;
}

/* --------------------------------------------------------------------------
 * Latest positions
 * ------------------------------------------------------------------------ */

/* One POST of the area query; returns the HTTP status, or -1 on transport error. */
static int post_latest(const char *body, int body_len)
{
    resp_reset();
    if (s_client == NULL) {
        esp_http_client_config_t cfg = {
            .url = AIS_LATEST_URL,
            .method = HTTP_METHOD_POST,
            .event_handler = http_event_handler,
            .timeout_ms = AIS_HTTP_TIMEOUT_MS,
            .keep_alive_enable = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
            /* The request headers carry the bearer token (a JWT, over 1 KB);
             * the default 512-byte send buffer truncates them. */
            .buffer_size_tx = AIS_TOKEN_MAX + 512,
        };
        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) {
            return -1;
        }
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_header(s_client, "User-Agent", CONFIG_EXAMPLE_YR_USER_AGENT);
    }

    char *auth = heap_caps_malloc(strlen(s_token) + 8, MALLOC_CAP_SPIRAM);
    if (auth == NULL) {
        return -1;
    }
    sprintf(auth, "Bearer %s", s_token);
    esp_http_client_set_header(s_client, "Authorization", auth);
    free(auth);
    esp_http_client_set_post_field(s_client, body, body_len);

    esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed: %s", esp_err_to_name(err));
        ais_client_close();
        return -1;
    }
    return esp_http_client_get_status_code(s_client);
}

esp_err_t ais_client_fetch(const char *client_id, const char *client_secret,
                           double lat, double lon, float radius_km, uint16_t min_len_m,
                           ais_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (client_id == NULL || client_id[0] == '\0' || client_secret == NULL || client_secret[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* A square around the centre, just enclosing the circle. */
    double dlat = radius_km / 110.574;
    double dlon = radius_km / (111.320 * cos(lat * M_PI / 180.0));
    char since[48] = "";
    time_t now = time(NULL);
    if (now > 1700000000) { /* clock synced */
        time_t t = now - AIS_SINCE_S;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(since, sizeof(since), ",\"since\":\"%Y-%m-%dT%H:%M:%SZ\"", &tm);
    }
    char body[400];
    int body_len = snprintf(body, sizeof(body),
        "{\"modelType\":\"%s\",\"modelFormat\":\"Json\"%s,"
        "\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[["
        "[%.5f,%.5f],[%.5f,%.5f],[%.5f,%.5f],[%.5f,%.5f],[%.5f,%.5f]]]}}",
        /* Only the Full model carries shipLength (at a few times the size). */
        min_len_m > 0 ? "Full" : "Simple", since,
        lon - dlon, lat - dlat, lon + dlon, lat - dlat, lon + dlon, lat + dlat,
        lon - dlon, lat + dlat, lon - dlon, lat - dlat);

    int status = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!token_valid()) {
            esp_err_t err = fetch_token(client_id, client_secret);
            if (err != ESP_OK) {
                return err == ESP_ERR_INVALID_STATE ? err : ESP_FAIL;
            }
        }
        status = post_latest(body, body_len);
        if (status == 401 || status == 403) {
            s_token[0] = '\0'; /* expired or revoked early: get a new one */
            continue;
        }
        if (status == -1 && attempt == 0) {
            continue; /* kept-alive connection dropped by the server: reconnect once */
        }
        break;
    }
    if (status == 401 || status == 403) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status != 200 || s_resp.buf == NULL) {
        ESP_LOGW(TAG, "fetch failed: status=%d %.300s", status, s_resp.buf ? s_resp.buf : "");
        ais_client_close();
        return ESP_FAIL;
    }

    if (!parse_response(s_resp.buf, s_resp.len, lat, lon, radius_km, min_len_m, out)) {
        ESP_LOGW(TAG, "unparseable response (%u bytes)", (unsigned)s_resp.len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%d ships in range, kept %d (%u bytes)", out->total, out->count,
             (unsigned)s_resp.len);
    return ESP_OK;
}
