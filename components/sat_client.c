#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "sat_client.h"
#include "yr_client.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "png.h"

static const char *TAG = "sat_client";

#define SAT_URL             "https://api.met.no/weatherapi/geosatellite/1.4/?area=europe&type="
#define SAT_HTTP_TIMEOUT_MS 20000
#define SAT_PIECE           4096
#define SAT_MAX_LEN         (4 * 1024 * 1024) /* the image is about 1 MB */

typedef struct {
    http_cache_t cache; /* from this reply's headers */
    time_t taken;
} sat_headers_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    sat_headers_t *h = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }
    if (strcasecmp(evt->header_key, "Last-Modified") == 0) {
        snprintf(h->cache.last_modified, sizeof(h->cache.last_modified), "%s", evt->header_value);
    } else if (strcasecmp(evt->header_key, "Expires") == 0) {
        h->cache.expires = http_date_to_epoch(evt->header_value);
    } else if (strcasecmp(evt->header_key, "Content-Disposition") == 0) {
        h->taken = sat_image_time(evt->header_value);
    }
    return ESP_OK;
}

/* The decoder's state, handed to libpng's callbacks. */
typedef struct {
    sat_row_fn row;
    void *ctx;
    int rows;  /* handed on so far */
    bool done; /* the end of the image was reached */
} sat_dec_t;

/* libpng's own memory - the inflate window and row buffers - in PSRAM:
 * TLS needs the internal RAM while the image streams in. */
static png_voidp png_psram_malloc(png_structp png, png_alloc_size_t n)
{
    (void)png;
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static void png_psram_free(png_structp png, png_voidp p)
{
    (void)png;
    heap_caps_free(p);
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

static void info_cb(png_structp png, png_infop info)
{
    const uint32_t w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    if (w != SAT_IMG_W || h != SAT_IMG_H || png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
        /* The projection in sat_util.c was fitted to this size. */
        ESP_LOGE(TAG, "The image is %ux%u%s, expected %dx%d", (unsigned)w, (unsigned)h,
                 png_get_interlace_type(png, info) != PNG_INTERLACE_NONE ? " interlaced" : "", SAT_IMG_W,
                 SAT_IMG_H);
        png_error(png, "unexpected image");
    }
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_strip_alpha(png);
    png_set_gray_to_rgb(png);
    png_read_update_info(png, info);
}

static void row_cb(png_structp png, png_bytep row, png_uint_32 y, int pass)
{
    (void)pass;
    sat_dec_t *d = png_get_progressive_ptr(png);
    if (row != NULL && (int)y == d->rows) {
        d->row((int)y, row, d->ctx);
        d->rows++;
    }
}

static void end_cb(png_structp png, png_infop info)
{
    (void)info;
    sat_dec_t *d = png_get_progressive_ptr(png);
    d->done = (d->rows == SAT_IMG_H);
}

/* Pass a piece of the file to libpng; false if it gave up on the image. */
static bool png_feed(png_structp png, png_infop info, png_bytep data, size_t n)
{
    if (setjmp(png_jmpbuf(png))) {
        return false;
    }
    png_process_data(png, info, data, n);
    return true;
}

/* Read the reply's body into the decoder until the image is complete. */
static esp_err_t decode_reply(esp_http_client_handle_t client, sat_dec_t *d)
{
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL, png_fail, png_warn, NULL,
                                               png_psram_malloc, png_psram_free);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    png_bytep piece = heap_caps_malloc(SAT_PIECE, MALLOC_CAP_SPIRAM);
    if (png == NULL || info == NULL || piece == NULL) {
        png_destroy_read_struct(&png, &info, NULL);
        heap_caps_free(piece);
        return ESP_ERR_NO_MEM;
    }
    png_set_progressive_read_fn(png, d, info_cb, row_cb, end_cb);

    esp_err_t err = ESP_OK;
    size_t total = 0;
    while (!d->done) {
        const int n = esp_http_client_read(client, (char *)piece, SAT_PIECE);
        if (n <= 0 && d->rows == SAT_IMG_H) {
            break; /* every row is in: what's left is only the file's end */
        }
        if (n <= 0) {
            ESP_LOGE(TAG, "The reply ended after %u bytes, %d rows", (unsigned)total, d->rows);
            err = ESP_FAIL;
            break;
        }
        total += (size_t)n;
        if (total > SAT_MAX_LEN || !png_feed(png, info, piece, (size_t)n)) {
            err = ESP_FAIL;
            break;
        }
    }
    png_destroy_read_struct(&png, &info, NULL);
    heap_caps_free(piece);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%u bytes", (unsigned)total);
    }
    return err;
}

esp_err_t sat_client_fetch(bool visible, http_cache_t *cache, sat_row_fn row, void *ctx, time_t *taken, int *rows)
{
    *taken = 0;
    *rows = 0;
    if (cache->expires != 0 && time(NULL) < cache->expires) {
        return HTTP_NOT_MODIFIED; /* MET asks not to ask again before then */
    }
    sat_headers_t h = { 0 };
    const esp_http_client_config_t config = {
        .url = visible ? SAT_URL "visible" : SAT_URL "infrared",
        .event_handler = http_event_handler,
        .user_data = &h,
        .timeout_ms = SAT_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "User-Agent", yr_client_user_agent());
    if (cache->last_modified[0] != '\0') {
        esp_http_client_set_header(client, "If-Modified-Since", cache->last_modified);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    int status = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    } else if (status == 304) {
        cache->expires = h.cache.expires;
        err = HTTP_NOT_MODIFIED;
    } else if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status %d", status);
        err = ESP_FAIL;
    } else {
        sat_dec_t d = { .row = row, .ctx = ctx };
        err = decode_reply(client, &d);
        *rows = d.rows;
        if (err == ESP_OK) {
            *cache = h.cache;
            *taken = h.taken;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
