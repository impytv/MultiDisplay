#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "adsb_client.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "adsb_client";

#define ADSB_HTTP_TIMEOUT_MS  10000
#define KM_PER_NM             1.852f
/* ~500 bytes per aircraft; even a busy 185 km circle stays well inside this. */
#define ADSB_MAX_RESPONSE_LEN (384 * 1024)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static resp_buf_t s_resp;
static esp_http_client_handle_t s_client;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (s_resp.len + evt->data_len + 1 > ADSB_MAX_RESPONSE_LEN) {
        ESP_LOGE(TAG, "Response too large, aborting");
        return ESP_FAIL;
    }
    if (s_resp.len + evt->data_len + 1 > s_resp.cap) {
        size_t new_cap = s_resp.cap ? s_resp.cap * 2 : 16 * 1024;
        while (new_cap < s_resp.len + evt->data_len + 1) {
            new_cap *= 2;
        }
        /* PSRAM: same reasoning as yr_client - TLS already fights for
         * internal DRAM while the body streams in. */
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

void adsb_client_close(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    free(s_resp.buf);
    s_resp = (resp_buf_t){ 0 };
}

/* --------------------------------------------------------------------------
 * A small, allocation-free scanner for the readsb-style JSON
 * ({"ac":[{...},{...}],...}). cJSON is unsuitable here: it allocates a small
 * node per field, and small allocations land in internal DRAM in this build
 * - a busy response would need hundreds of KB of it. Nested objects/arrays
 * inside an aircraft (lastPosition, mlat, tisb) are skipped, not searched, so
 * their "lat"/"lon" can never be mistaken for the top-level position.
 * ------------------------------------------------------------------------ */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
    }
    return p;
}

/* p points at an opening '"'; returns one past the closing quote (or end). */
static const char *skip_string(const char *p, const char *end)
{
    p++;
    while (p < end && *p != '"') {
        p += (*p == '\\' && p + 1 < end) ? 2 : 1;
    }
    return p < end ? p + 1 : end;
}

/* p points at '{' or '['; returns one past the matching close. */
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

/* p points at the first char of a value; returns one past it. */
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

typedef struct {
    bool has_lat, has_lon, has_dst, has_dir;
    bool on_ground;
    bool has_alt;
    float lat, lon; /* only checked for presence */
    float alt, gs, track, true_heading, mag_heading, dst, dir, seen_pos;
    bool has_track, has_true_heading, has_mag_heading;
    char flight[sizeof(((adsb_aircraft_t *)0)->callsign)], reg[sizeof(((adsb_aircraft_t *)0)->callsign)],
        hex[sizeof(((adsb_aircraft_t *)0)->callsign)], type[sizeof(((adsb_aircraft_t *)0)->type)];
} raw_ac_t;

static void copy_trimmed(char *dst, size_t dst_len, const char *src, size_t n)
{
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    while (n > 0 && src[n - 1] == ' ') {
        n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static float to_float(const char *p, const char *end)
{
    char tmp[24];
    size_t n = (size_t)(end - p);
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    return strtof(tmp, NULL);
}

static bool is_number_start(char c)
{
    return c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9');
}

/* Parse one aircraft object [p, end) where *p == '{'. */
static void parse_aircraft(const char *p, const char *end, raw_ac_t *r)
{
    memset(r, 0, sizeof(*r));
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
        const char *key_end = skip_string(p, end) - 1; /* the closing quote */
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
        if (KEY_IS("alt_baro")) {
            if (is_str) {
                r->on_ground = (slen == 6 && memcmp(sv, "ground", 6) == 0);
            } else if (is_num) {
                r->alt = to_float(val, val_end);
                r->has_alt = true;
            }
        } else if (KEY_IS("lat") && is_num) {
            r->has_lat = true;
        } else if (KEY_IS("lon") && is_num) {
            r->has_lon = true;
        } else if (KEY_IS("dst") && is_num) {
            r->dst = to_float(val, val_end);
            r->has_dst = true;
        } else if (KEY_IS("dir") && is_num) {
            r->dir = to_float(val, val_end);
            r->has_dir = true;
        } else if (KEY_IS("gs") && is_num) {
            r->gs = to_float(val, val_end);
        } else if (KEY_IS("track") && is_num) {
            r->track = to_float(val, val_end);
            r->has_track = true;
        } else if (KEY_IS("true_heading") && is_num) {
            r->true_heading = to_float(val, val_end);
            r->has_true_heading = true;
        } else if (KEY_IS("mag_heading") && is_num) {
            r->mag_heading = to_float(val, val_end);
            r->has_mag_heading = true;
        } else if (KEY_IS("seen_pos") && is_num) {
            r->seen_pos = to_float(val, val_end);
        } else if (KEY_IS("flight") && is_str) {
            copy_trimmed(r->flight, sizeof(r->flight), sv, slen);
        } else if (KEY_IS("r") && is_str) {
            copy_trimmed(r->reg, sizeof(r->reg), sv, slen);
        } else if (KEY_IS("hex") && is_str) {
            copy_trimmed(r->hex, sizeof(r->hex), sv, slen);
        } else if (KEY_IS("t") && is_str) {
            copy_trimmed(r->type, sizeof(r->type), sv, slen);
        }
#undef KEY_IS
    }
}

static bool parse_response(const char *json, size_t len, adsb_result_t *out)
{
    const char *end = json + len;
    const char *ac = strstr(json, "\"ac\":");
    if (ac == NULL) {
        return false;
    }
    const char *p = skip_ws(ac + 5, end);
    if (p >= end || *p != '[') {
        return false;
    }
    p++;

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

        raw_ac_t r;
        parse_aircraft(p, obj_end, &r);
        p = obj_end;

        if (r.on_ground || !r.has_lat || !r.has_lon || !r.has_dst || !r.has_dir) {
            continue;
        }
        out->total++;

        adsb_aircraft_t a = { 0 };
        snprintf(a.callsign, sizeof(a.callsign), "%s",
                 r.flight[0] ? r.flight : (r.reg[0] ? r.reg : r.hex));
        snprintf(a.type, sizeof(a.type), "%s", r.type);
        a.alt_ft = r.has_alt ? (int32_t)lroundf(r.alt) : ADSB_ALT_UNKNOWN;
        a.dist_km = r.dst * KM_PER_NM;
        a.bearing_deg = r.dir;
        a.gs_kt = r.gs;
        a.track_deg = r.has_track ? r.track
                    : (r.has_true_heading ? r.true_heading
                    : (r.has_mag_heading ? r.mag_heading : 0.0f));
        a.seen_pos_s = r.seen_pos;

        /* Insert into the nearest-first list, dropping the farthest when full. */
        int at = out->count;
        if (out->count == ADSB_MAX_AIRCRAFT) {
            if (a.dist_km >= out->ac[out->count - 1].dist_km) {
                continue;
            }
            at = out->count - 1;
        } else {
            out->count++;
        }
        while (at > 0 && out->ac[at - 1].dist_km > a.dist_km) {
            out->ac[at] = out->ac[at - 1];
            at--;
        }
        out->ac[at] = a;
    }
    return true;
}

esp_err_t adsb_client_fetch(double lat, double lon, float radius_km, adsb_result_t *out)
{
    memset(out, 0, sizeof(*out));

    char url[128];
    snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%.1f",
             lat, lon, (double)(radius_km / KM_PER_NM));

    s_resp.len = 0; /* keep the buffer (PSRAM) for reuse between polls */
    if (s_resp.buf != NULL) {
        s_resp.buf[0] = '\0';
    }

    if (s_client == NULL) {
        /* No cert/CA configured -> esp-tls skips verification, as yr_client
         * does (see the note there): public, read-only data. */
        esp_http_client_config_t cfg = {
            .url = url,
            .event_handler = http_event_handler,
            .timeout_ms = ADSB_HTTP_TIMEOUT_MS,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) {
            return ESP_FAIL;
        }
        esp_http_client_set_header(s_client, "User-Agent", CONFIG_EXAMPLE_YR_USER_AGENT);
    } else {
        esp_http_client_set_url(s_client, url);
    }

    esp_err_t err = esp_http_client_perform(s_client);
    int status = esp_http_client_get_status_code(s_client);
    if (err != ESP_OK || status != 200 || s_resp.buf == NULL) {
        ESP_LOGW(TAG, "fetch failed: err=%s status=%d", esp_err_to_name(err), status);
        /* The connection may be half-dead (server closed it, 429, ...): start
         * clean next time. */
        adsb_client_close();
        return ESP_FAIL;
    }

    if (!parse_response(s_resp.buf, s_resp.len, out)) {
        ESP_LOGW(TAG, "unparseable response (%u bytes)", (unsigned)s_resp.len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%d aircraft in range, kept %d (%u bytes)", out->total, out->count,
             (unsigned)s_resp.len);
    return ESP_OK;
}
