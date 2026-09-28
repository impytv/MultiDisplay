/* Screenshot: GET /screen.png on the config web server returns the current
 * screen as a PNG. The image data is stored uncompressed (deflate "stored"
 * blocks, one per row), which needs no compressor and streams row by row;
 * about 1.1 MB for 800 x 480. */

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_rom_crc.h"

#include "app.h"
#include "screenshot.h"
#include "waveshare_rgb_lcd_port.h"

/* PSRAM left over for everything else while a screenshot is taken. */
#define SCREENSHOT_PSRAM_SPARE (448 * 1024)

typedef struct {
    httpd_req_t *req;
    uint8_t buf[2048];
    size_t n;
    uint32_t crc;   /* of the PNG chunk being written */
    esp_err_t err;
} png_out_t;

static void png_put(png_out_t *o, const void *data, size_t len)
{
    const uint8_t *p = data;
    o->crc = esp_rom_crc32_le(o->crc, p, len);
    while (len > 0 && o->err == ESP_OK) {
        size_t k = sizeof(o->buf) - o->n;
        k = k < len ? k : len;
        memcpy(o->buf + o->n, p, k);
        o->n += k;
        p += k;
        len -= k;
        if (o->n == sizeof(o->buf)) {
            o->err = httpd_resp_send_chunk(o->req, (const char *)o->buf, o->n);
            o->n = 0;
        }
    }
}

static void png_put_u32(png_out_t *o, uint32_t v)
{
    uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };
    png_put(o, b, 4);
}

static void png_chunk_start(png_out_t *o, const char *type, uint32_t len)
{
    png_put_u32(o, len);
    o->crc = 0;
    png_put(o, type, 4);
}

static void png_chunk_end(png_out_t *o)
{
    png_put_u32(o, o->crc);
}

esp_err_t screenshot_handler(httpd_req_t *req)
{
    /* The snapshot is a full-screen RGB565 copy (750 KB of PSRAM). Refuse
     * rather than take it when that would leave too little for the fetches
     * running meanwhile: a forecast parse needs ~350 KB. (A coastline render
     * needs more, but only on the first visit to a location's radar; if it
     * can't get it, that coastline is drawn on the next visit instead.) */
    const size_t snap_bytes = (size_t)EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < snap_bytes ||
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < snap_bytes + SCREENSHOT_PSRAM_SPARE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Not enough free memory for a screenshot right now\n");
    }
    lv_draw_buf_t *snap = NULL;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        esp_lv_adapter_unlock();
    }
    png_out_t *o = heap_caps_calloc(1, sizeof(*o), MALLOC_CAP_SPIRAM);
    uint8_t *row = heap_caps_malloc(1 + 3 * (snap ? snap->header.w : 0), MALLOC_CAP_SPIRAM);
    if (snap == NULL || o == NULL || row == NULL) {
        if (snap != NULL) {
            lv_draw_buf_destroy(snap);
        }
        free(o);
        free(row);
        return httpd_resp_send_500(req);
    }
    const uint32_t w = snap->header.w, h = snap->header.h;
    const uint32_t row_len = 1 + 3 * w; /* filter byte + RGB */
    o->req = req;
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screen.png\"");

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    png_put(o, sig, sizeof(sig));
    png_chunk_start(o, "IHDR", 13);
    png_put_u32(o, w);
    png_put_u32(o, h);
    static const uint8_t ihdr[5] = { 8, 2, 0, 0, 0 }; /* 8-bit RGB, no interlace */
    png_put(o, ihdr, sizeof(ihdr));
    png_chunk_end(o);

    png_chunk_start(o, "IDAT", 2 + h * (5 + row_len) + 4);
    static const uint8_t zhdr[2] = { 0x78, 0x01 };
    png_put(o, zhdr, sizeof(zhdr));
    uint32_t a1 = 1, a2 = 0; /* Adler-32 of the raw rows */
    for (uint32_t y = 0; y < h && o->err == ESP_OK; y++) {
        const uint8_t *src = snap->data + y * snap->header.stride;
        row[0] = 0; /* no filter */
        for (uint32_t x = 0; x < w; x++) { /* RGB565, little-endian */
            const uint16_t v = (uint16_t)(src[2 * x] | (src[2 * x + 1] << 8));
            const uint8_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            row[1 + 3 * x] = (uint8_t)((r << 3) | (r >> 2));
            row[2 + 3 * x] = (uint8_t)((g << 2) | (g >> 4));
            row[3 + 3 * x] = (uint8_t)((b << 3) | (b >> 2));
        }
        for (uint32_t i = 0; i < row_len; i++) {
            a1 = (a1 + row[i]) % 65521;
            a2 = (a2 + a1) % 65521;
        }
        uint8_t bh[5] = { y == h - 1, row_len & 0xFF, row_len >> 8, ~row_len & 0xFF, (~row_len >> 8) & 0xFF };
        png_put(o, bh, sizeof(bh));
        png_put(o, row, row_len);
    }
    png_put_u32(o, (a2 << 16) | a1);
    png_chunk_end(o);
    png_chunk_start(o, "IEND", 0);
    png_chunk_end(o);

    lv_draw_buf_destroy(snap);
    free(row);
    esp_err_t err = o->err;
    if (err == ESP_OK && o->n > 0) {
        err = httpd_resp_send_chunk(req, (const char *)o->buf, o->n);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    free(o);
    return err;
}
