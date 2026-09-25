#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rain_client.h"
#include "yr_client.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "png.h"

static const char *TAG = "rain_client";

#define RAIN_HTTP_TIMEOUT_MS  20000
#define RAIN_MAX_RESPONSE_LEN (1536 * 1024) /* the images are 150-300 KB */
/* The legend panel down the right-hand side of every image. */
#define RAIN_LEGEND_W         90

#define EARTH_R_M 6371000.0

/* Fitted by scripts/fit_radar_areas.py against the coastline; regenerate
 * with it if MET changes the images (their size is checked on every fetch). */
static const rain_area_t AREAS[] = {
    { "southeastern_norway", 771, 631, 58.923f, 8.936f, 997.03f, -350.68f, 3527.10f },
    { "eastern_norway", 579, 677, 59.785f, 10.512f, 997.56f, -247.17f, 3305.25f },
    { "southern_norway", 890, 919, 60.368f, 7.748f, 997.97f, -402.44f, 3181.65f },
    { "southwestern_norway", 696, 664, 59.686f, 6.021f, 997.75f, -300.07f, 3461.96f },
    { "western_norway", 579, 758, 60.965f, 5.116f, 998.10f, -249.36f, 3154.58f },
    { "central_norway", 911, 833, 62.675f, 7.773f, 997.26f, -385.37f, 2765.54f },
    { "southern_nordland", 663, 661, 64.516f, 11.226f, 997.12f, -298.50f, 2721.77f },
    { "nordland", 580, 756, 66.444f, 12.086f, 997.20f, -251.47f, 2411.29f },
    { "northern_nordland", 737, 680, 68.381f, 14.255f, 996.72f, -339.48f, 2192.00f },
    { "troms", 870, 642, 69.965f, 19.410f, 996.56f, -382.47f, 2012.43f },
    { "finnmark", 820, 653, 70.452f, 25.700f, 996.38f, -368.41f, 1999.48f },
    { "norway", 726, 1037, 64.480f, -0.017f, 2002.63f, 5.73f, 936.30f },
    { "nordic", 659, 761, 63.247f, 15.006f, 3009.64f, -271.53f, 694.12f },
};

static void area_project(const rain_area_t *a, double lat, double lon, double *px, double *py)
{
    const double d2r = M_PI / 180.0;
    const double p1 = a->lat1 * d2r;
    const double n = sin(p1);
    const double f = cos(p1) * pow(tan(M_PI / 4 + p1 / 2), n) / n;
    const double rho = EARTH_R_M * f / pow(tan(M_PI / 4 + lat * d2r / 2), n);
    const double th = n * (lon - a->lon0) * d2r;
    *px = rho * sin(th) / a->m_per_px - a->ox;
    *py = rho * cos(th) / a->m_per_px - a->oy;
}

void rain_client_project(const rain_area_t *area, double lat, double lon, float *px, float *py)
{
    double x, y;
    area_project(area, lat, lon, &x, &y);
    *px = (float)x;
    *py = (float)y;
}

const rain_area_t *rain_client_pick_area(double lat, double lon, float range_km)
{
    const rain_area_t *best = NULL;
    double best_slack = 0;
    bool best_fits = false;
    for (size_t i = 0; i < sizeof(AREAS) / sizeof(AREAS[0]); i++) {
        const rain_area_t *a = &AREAS[i];
        double x, y;
        area_project(a, lat, lon, &x, &y);
        /* How far (km) the circle stays inside the map part of the image;
         * negative where it spills over. */
        double edge_px = fmin(fmin(x, a->w - RAIN_LEGEND_W - x), fmin(y, a->h - y));
        double slack = edge_px * a->m_per_px / 1000.0 - range_km;
        if (edge_px <= 0) {
            continue; /* the point itself isn't on this one */
        }
        bool fits = slack >= 0;
        bool better;
        if (best == NULL) {
            better = true;
        } else if (fits != best_fits) {
            better = fits;
        } else if (fits && a->m_per_px != best->m_per_px) {
            better = a->m_per_px < best->m_per_px; /* the sharpest one that fits */
        } else {
            better = slack > best_slack;
        }
        if (better) {
            best = a;
            best_slack = slack;
            best_fits = fits;
        }
    }
    return best;
}

/* The 5level_reflectivity palette, blended a little into the map under it:
 * light green, green, yellow, orange, red. Grey terrain, the bluish sea and
 * lakes, the white borders and the dark labels match none of them. */
static uint8_t rain_level(int r, int g, int b)
{
    const int e = r - g, d = g - b;
    if (e > 110 && g < 110 && b < 110) {
        return 5;
    }
    if (e > 50 && d > 70) {
        return 4;
    }
    if (d > 150 && e > -25 && e < 25) {
        return 3;
    }
    if (e < -20 && d >= 68 && d < 110) {
        return 2;
    }
    if (e < -20 && d >= 40 && d < 68) {
        return 1;
    }
    return 0;
}

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    time_t time;
} rain_resp_t;

/* Days since 1970-01-01 of a proleptic Gregorian date. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* The image time is in the file name: Content-Disposition: inline;
 * filename="web5color-sorostnorge_20260924T203000Z.png". */
static time_t parse_image_time(const char *v)
{
    const char *u = strrchr(v, '_');
    int y, mo, d, h, mi, s;
    if (u == NULL || sscanf(u + 1, "%4d%2d%2dT%2d%2d%2dZ", &y, &mo, &d, &h, &mi, &s) != 6) {
        return 0;
    }
    return (time_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    rain_resp_t *resp = (rain_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "Content-Disposition") == 0) {
            resp->time = parse_image_time(evt->header_value);
        }
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (resp->len + evt->data_len > RAIN_MAX_RESPONSE_LEN) {
        ESP_LOGE(TAG, "Response too large, aborting");
        return ESP_FAIL;
    }
    if (resp->len + evt->data_len > resp->cap) {
        /* Doubling, not a chunk at a time: fewer copies, and PSRAM left in
         * big pieces rather than chopped up. */
        size_t cap = resp->cap ? resp->cap * 2 : 64 * 1024;
        while (cap < resp->len + evt->data_len) {
            cap *= 2;
        }
        uint8_t *nb = heap_caps_realloc(resp->buf, cap, MALLOC_CAP_SPIRAM);
        if (nb == NULL) {
            ESP_LOGE(TAG, "Out of memory growing response buffer to %u (PSRAM free %u, largest %u)",
                     (unsigned)cap, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return ESP_FAIL;
        }
        resp->buf = nb;
        resp->cap = cap;
    }
    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += evt->data_len;
    return ESP_OK;
}

typedef struct {
    const uint8_t *p;
    size_t len, pos;
} png_src_t;

static void png_mem_read(png_structp png, png_bytep out, size_t n)
{
    png_src_t *s = (png_src_t *)png_get_io_ptr(png);
    if (s->pos + n > s->len) {
        png_error(png, "truncated");
    }
    memcpy(out, s->p + s->pos, n);
    s->pos += n;
}

static void png_warn(png_structp png, png_const_charp msg)
{
    (void)png;
    ESP_LOGD(TAG, "libpng: %s", msg);
}

static void png_fail(png_structp png, png_const_charp msg)
{
    ESP_LOGE(TAG, "libpng: %s", msg);
    png_longjmp(png, 1);
}

/* Decode the PNG row by row straight into the crop's rain levels, so
 * neither the RGB image nor the whole area's levels ever have to be held. */
static esp_err_t decode_levels(const rain_area_t *a, const rain_crop_t *c, const uint8_t *data, size_t len,
                               uint8_t *level)
{
    png_src_t src = { .p = data, .len = len };
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_fail, png_warn);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    uint8_t *volatile row = NULL;
    if (info == NULL) {
        png_destroy_read_struct(&png, NULL, NULL);
        return ESP_ERR_NO_MEM;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        heap_caps_free(row);
        return ESP_FAIL;
    }
    png_set_read_fn(png, &src, png_mem_read);
    png_read_info(png, info);
    const uint32_t w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    if (w != a->w || h != a->h || png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
        ESP_LOGE(TAG, "%s: got a %ux%u%s image, expected %ux%u", a->name, (unsigned)w, (unsigned)h,
                 png_get_interlace_type(png, info) != PNG_INTERLACE_NONE ? " interlaced" : "",
                 a->w, a->h);
        png_destroy_read_struct(&png, &info, NULL);
        return ESP_ERR_INVALID_SIZE;
    }
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_strip_alpha(png);
    png_set_gray_to_rgb(png);
    png_read_update_info(png, info);
    row = heap_caps_malloc(png_get_rowbytes(png, info), MALLOC_CAP_SPIRAM);
    if (row == NULL) {
        png_destroy_read_struct(&png, &info, NULL);
        return ESP_ERR_NO_MEM;
    }
    memset(level, 0, (size_t)c->w * c->h);
    const int map_w = (int)w - RAIN_LEGEND_W;
    const int xa = c->x0 > 0 ? c->x0 : 0;
    const int xb = c->x0 + c->w * c->step < map_w ? c->x0 + c->w * c->step : map_w;
    for (int y = 0; y < (int)h; y++) {
        png_read_row(png, row, NULL); /* every row: libpng can't skip */
        const int cy = (y - c->y0) / c->step;
        if (y < c->y0 || cy >= c->h) {
            continue;
        }
        uint8_t *out = level + (size_t)cy * c->w;
        for (int x = xa; x < xb; x++) {
            const uint8_t *px = row + 3 * x;
            const uint8_t l = rain_level(px[0], px[1], px[2]);
            uint8_t *o = &out[(x - c->x0) / c->step];
            if (l > *o) {
                *o = l;
            }
        }
    }
    png_destroy_read_struct(&png, &info, NULL);
    heap_caps_free(row);
    return ESP_OK;
}

static esp_http_client_handle_t s_client;
static rain_resp_t s_resp;

void rain_client_close(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

esp_err_t rain_client_fetch(const rain_area_t *area, time_t when, const rain_crop_t *crop,
                            uint8_t *level, time_t *taken)
{
    char url[192];
    int n = snprintf(url, sizeof(url),
                     "https://api.met.no/weatherapi/radar/2.0/?area=%s&type=5level_reflectivity", area->name);
    if (when > 0) {
        struct tm t;
        gmtime_r(&when, &t);
        snprintf(url + n, sizeof(url) - n, "&time=%04d-%02d-%02dT%02d%%3A%02d%%3A00Z",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    }

    s_resp = (rain_resp_t){ 0 };
    if (s_client == NULL) {
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .user_data = &s_resp,
            .timeout_ms = RAIN_HTTP_TIMEOUT_MS,
            .buffer_size = 4096,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&config);
        if (s_client == NULL) {
            return ESP_FAIL;
        }
        esp_http_client_set_header(s_client, "User-Agent", yr_client_user_agent());
    } else {
        esp_http_client_set_url(s_client, url);
    }
    esp_err_t err = esp_http_client_perform(s_client);
    int status = esp_http_client_get_status_code(s_client);
    rain_resp_t resp = s_resp;
    s_resp = (rain_resp_t){ 0 };
    if (err != ESP_OK || status != 200 || resp.buf == NULL) {
        ESP_LOGE(TAG, "%s: HTTP %s, status %d", area->name, esp_err_to_name(err), status);
        heap_caps_free(resp.buf);
        if (err != ESP_OK || status != 404) {
            rain_client_close(); /* may be half-dead: start clean next time */
        }
        return err != ESP_OK ? err : (status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL);
    }

    err = decode_levels(area, crop, resp.buf, resp.len, level);
    heap_caps_free(resp.buf);
    if (err != ESP_OK) {
        return err;
    }
    *taken = resp.time ? resp.time : when;
    ESP_LOGI(TAG, "%s: %u bytes, image time %lld", area->name, (unsigned)resp.len, (long long)*taken);
    return ESP_OK;
}
