/* Screenshot: GET /screen.png on the config web server returns the current
 * screen as a PNG. The image data is stored uncompressed (deflate "stored"
 * blocks, one per row), which needs no compressor and streams row by row;
 * about 1.1 MB for 800 x 480. */

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

#include "app.h"
#include "screenshot.h"
#include "waveshare_rgb_lcd_port.h"

/* The longest drawing is held off for a screenshot. */
#define SCREENSHOT_MAX_US (20 * 1000 * 1000)

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

/* The panel's frame buffers, and which was last handed to it to show: the
 * display adapter draws into the others and then hands one over. */
static void *s_fbs[3];
static int s_fb_count;
static const void *volatile s_front;
static esp_err_t (*s_draw_bitmap)(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                  const void *color_data);

static esp_err_t draw_bitmap_hook(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                  const void *color_data)
{
    for (int i = 0; i < s_fb_count; i++) {
        if (color_data == s_fbs[i]) {
            s_front = color_data;
        }
    }
    return s_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
}

void screenshot_init(esp_lcd_panel_handle_t panel, int fb_count)
{
    s_fb_count = fb_count < 3 ? fb_count : 3;
    if (s_fb_count < 1 || esp_lcd_rgb_panel_get_frame_buffer(panel, s_fb_count, &s_fbs[0], &s_fbs[1], &s_fbs[2]) !=
                              ESP_OK) {
        s_fb_count = 0;
        return;
    }
    s_draw_bitmap = panel->draw_bitmap;
    panel->draw_bitmap = draw_bitmap_hook;
}

esp_err_t screenshot_handler(httpd_req_t *req)
{
    /* Read from the frame on show rather than from a copy (750 KB of PSRAM
     * that a busy display may not have). Drawing is held off meanwhile, so
     * the adapter doesn't start reusing the frame - for a few seconds at
     * most: a slow client is cut off after SCREENSHOT_MAX_US. */
    if (s_front == NULL || esp_lv_adapter_lock(-1) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Nothing on the screen yet\n");
    }
    const uint8_t *fb = s_front;
    png_out_t *o = heap_caps_calloc(1, sizeof(*o), MALLOC_CAP_SPIRAM);
    uint8_t *row = heap_caps_malloc(1 + 3 * BOARD_LCD_H_RES, MALLOC_CAP_SPIRAM);
    if (o == NULL || row == NULL) {
        esp_lv_adapter_unlock();
        free(o);
        free(row);
        return httpd_resp_send_500(req);
    }
    const int64_t started = esp_timer_get_time();
    const uint32_t w = BOARD_LCD_H_RES, h = BOARD_LCD_V_RES;
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
        if (esp_timer_get_time() - started > SCREENSHOT_MAX_US) {
            o->err = ESP_ERR_TIMEOUT;
            break;
        }
        const uint8_t *src = fb + y * w * 2;
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

    esp_lv_adapter_unlock();
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
