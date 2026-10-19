/* Drawing into a custom draw callback's layer (the radar, ship and rain
 * screens and the departure board paint themselves this way rather than
 * with a widget per element). LVGL renders in horizontal strips, calling
 * the callback once per strip, so every primitive is culled against the
 * strip first: a draw task per element per strip would otherwise cost a lot
 * of transient internal DRAM. Text is in the body font. */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"

#include "app.h"
#include "draw.h"

bool draw_area_visible(const lv_layer_t *layer, const lv_area_t *a)
{
    const lv_area_t *c = &layer->_clip_area;
    return a->x1 <= c->x2 && a->x2 >= c->x1 && a->y1 <= c->y2 && a->y2 >= c->y1;
}

bool draw_visible(const lv_layer_t *layer, int x1, int y1, int x2, int y2)
{
    lv_area_t a = { x1, y1, x2, y2 };
    return draw_area_visible(layer, &a);
}

void draw_text(lv_layer_t *layer, const char *txt, int x, int y, int w,
                       lv_text_align_t align, lv_color_t color)
{
    int h = lv_font_get_line_height(g_font_body);
    if (!draw_visible(layer, x, y, x + w - 1, y + h - 1)) {
        return;
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = g_font_body;
    d.color = color;
    d.text = txt;
    d.text_local = 1; /* drawing is deferred; LVGL copies the string */
    d.align = align;
    lv_area_t a = { x, y, x + w - 1, y + h - 1 };
    lv_draw_label(layer, &d, &a);
}

void draw_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int width, lv_color_t color)
{
    int lo_x = x1 < x2 ? x1 : x2, hi_x = x1 < x2 ? x2 : x1;
    int lo_y = y1 < y2 ? y1 : y2, hi_y = y1 < y2 ? y2 : y1;
    if (!draw_visible(layer, lo_x - width, lo_y - width, hi_x + width, hi_y + width)) {
        return;
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = width;
    d.color = color;
    d.opa = LV_OPA_COVER;
    lv_draw_line(layer, &d);
}

void draw_triangle(lv_layer_t *layer, const int pts[3][2], lv_color_t color)
{
    int lo_x = pts[0][0], hi_x = pts[0][0], lo_y = pts[0][1], hi_y = pts[0][1];
    for (int i = 1; i < 3; i++) {
        if (pts[i][0] < lo_x) lo_x = pts[i][0];
        if (pts[i][0] > hi_x) hi_x = pts[i][0];
        if (pts[i][1] < lo_y) lo_y = pts[i][1];
        if (pts[i][1] > hi_y) hi_y = pts[i][1];
    }
    if (!draw_visible(layer, lo_x, lo_y, hi_x, hi_y)) {
        return;
    }
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    for (int i = 0; i < 3; i++) {
        d.p[i].x = pts[i][0];
        d.p[i].y = pts[i][1];
    }
    d.color = color;
    d.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &d);
}

void draw_dot(lv_layer_t *layer, int cx, int cy, int r, lv_color_t color)
{
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    if (!draw_area_visible(layer, &a)) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = color;
    d.border_width = 0;
    lv_draw_rect(layer, &d, &a);
}

void draw_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, int radius, lv_color_t color)
{
    lv_area_t a = { x1, y1, x2, y2 };
    if (!draw_area_visible(layer, &a)) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = radius;
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = color;
    d.border_width = 0;
    lv_draw_rect(layer, &d, &a);
}

void draw_text_shorten(const lv_font_t *font, const char *txt, int w, char *buf, size_t buf_len)
{
    const size_t full = strlen(txt);
    size_t len = full;
    if (len > buf_len - 3) {
        len = buf_len - 3;
    }
    /* Never cut inside a UTF-8 character (æ, ø, å are two bytes). */
    while (len > 0 && ((unsigned char)txt[len] & 0xC0) == 0x80) {
        len--;
    }
    for (size_t n = len;; n--) {
        while (n > 1 && n < len && ((unsigned char)txt[n] & 0xC0) == 0x80) {
            n--;
        }
        memcpy(buf, txt, n);
        if (n < full) {
            memcpy(buf + n, "..", 3);
        } else {
            buf[n] = '\0';
        }
        lv_point_t sz;
        lv_text_get_size(&sz, buf, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= w || n <= 1) {
            break;
        }
    }
}

/* Draw `txt` left-aligned in w px, shortened with ".." if it doesn't fit. */
void draw_text_fit(lv_layer_t *layer, const char *txt, int x, int y, int w, lv_color_t color)
{
    if (!draw_visible(layer, x, y, x + w - 1, y + lv_font_get_line_height(g_font_body) - 1)) {
        return;
    }
    char buf[128]; /* longer than anything drawn: a disruption notice */
    draw_text_shorten(g_font_body, txt, w, buf, sizeof(buf));
    draw_text(layer, buf, x, y, w, LV_TEXT_ALIGN_LEFT, color);
}

int draw_text_lines(const char *txt, int w, int max_lines)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, g_font_body, 0, 0, w, LV_TEXT_FLAG_NONE);
    const int lh = lv_font_get_line_height(g_font_body);
    const int n = (sz.y + lh - 1) / lh;
    return n < 1 ? 1 : n > max_lines ? max_lines : n;
}

void draw_text_wrap(lv_layer_t *layer, const char *txt, int x, int y, int w, int lines, lv_color_t color)
{
    if (lines <= 1) {
        draw_text_fit(layer, txt, x, y, w, color); /* one line, ".." if cut */
        return;
    }
    const int h = lines * lv_font_get_line_height(g_font_body);
    if (!draw_visible(layer, x, y, x + w - 1, y + h - 1)) {
        return;
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = g_font_body;
    d.color = color;
    d.text = txt;
    d.text_local = 1;
    d.align = LV_TEXT_ALIGN_LEFT;
    lv_area_t a = { x, y, x + w - 1, y + h - 1 };
    /* LVGL wraps, but draws the lines past the area too: clip to it (a draw
     * task takes the layer's clip area when it is created). */
    const lv_area_t clip = layer->_clip_area;
    const lv_area_t cut = { LV_MAX(clip.x1, a.x1), LV_MAX(clip.y1, a.y1), LV_MIN(clip.x2, a.x2),
                            LV_MIN(clip.y2, a.y2) };
    if (cut.x1 <= cut.x2 && cut.y1 <= cut.y2) {
        layer->_clip_area = cut;
        lv_draw_label(layer, &d, &a);
        layer->_clip_area = clip;
    }
}

int draw_text_w(const char *txt)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, g_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* The JPEG decoder's working memory. Its own allocator (esp_new_jpeg's
 * jpeg_calloc_inner and jpeg_calloc_align_inner) takes internal RAM while
 * there is any: about 30 KB for a 600-px picture's row of blocks, which
 * took the lowest free internal RAM from ~60 KB to ~28 KB - too near what
 * WiFi needs. Linked in their place (--wrap, main/CMakeLists.txt): small
 * pieces still internal, big ones from PSRAM, either falling back to the
 * other. */
#define JPEG_INTERNAL_MAX 4096

void *__wrap_jpeg_calloc_inner(size_t size)
{
    return size <= JPEG_INTERNAL_MAX
               ? heap_caps_calloc_prefer(1, size, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
               : heap_caps_calloc_prefer(1, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *__wrap_jpeg_calloc_align_inner(size_t size, int aligned)
{
    const uint32_t first = size <= JPEG_INTERNAL_MAX ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM;
    const uint32_t then = size <= JPEG_INTERNAL_MAX ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
    void *p = heap_caps_aligned_calloc((size_t)aligned, 1, size, first | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_aligned_calloc((size_t)aligned, 1, size, then | MALLOC_CAP_8BIT);
}

/* Decode jpg, scaled to sw x sh if those aren't 0, into *px (into `into` if
 * the result is exactly into_len bytes, else a new PSRAM buffer). */
static bool jpeg_decode(const uint8_t *jpg, size_t len, int sw, int sh, uint16_t *into, size_t into_len,
                        uint16_t **px, int *w, int *h)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.scale.width = (uint16_t)sw;
    cfg.scale.height = (uint16_t)sh;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = (int)len };
    jpeg_dec_header_info_t info;
    int out_len = 0;
    bool ok = false;
    *px = NULL;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && jpeg_dec_get_outbuf_len(dec, &out_len) == JPEG_ERR_OK) {
        *w = sw ? sw : info.width;
        *h = sh ? sh : info.height;
        if (into != NULL && (size_t)out_len == into_len) {
            *px = into;
        } else {
            *px = heap_caps_aligned_alloc(16, (size_t)out_len, MALLOC_CAP_SPIRAM);
        }
        if (*px != NULL && (size_t)out_len >= (size_t)*w * *h * 2) {
            io.outbuf = (uint8_t *)*px;
            ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
        }
    }
    jpeg_dec_close(dec);
    if (!ok && *px != into) {
        heap_caps_free(*px);
        *px = NULL;
    }
    return ok;
}

bool draw_jpeg_square(const uint8_t *jpg, size_t len, uint16_t *out, int side)
{
    /* The size first, to choose the scale. */
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_header_info_t info = { 0 };
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = (int)len };
    const bool head = jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK;
    jpeg_dec_close(dec);
    if (!head || info.width == 0 || info.height == 0 || info.width > 4000 || info.height > 4000) {
        ESP_LOGW("draw", "Not a JPEG to decode (%u bytes)", (unsigned)len);
        return false;
    }
    /* Scaled so the shorter side is `side` (in 8s), if smaller. */
    const int W = info.width, H = info.height, m = W < H ? W : H;
    int sw = 0, sh = 0;
    if (m > side && side % 8 == 0) {
        sw = (W * side / m + 7) & ~7;
        sh = (H * side / m + 7) & ~7;
        if (sw > W || sh > H) {
            sw = sh = 0;
        }
    }
    /* Straight into out if it comes out at side x side. */
    const size_t out_len = (size_t)side * side * 2;
    uint16_t *px;
    int w, h;
    bool ok = jpeg_decode(jpg, len, sw, sh, (sw ? sw : W) == side && (sh ? sh : H) == side ? out : NULL, out_len, &px,
                          &w, &h);
    if (!ok && sw) {
        ok = jpeg_decode(jpg, len, 0, 0, W == side && H == side ? out : NULL, out_len, &px, &w, &h); /* the scale refused */
    }
    if (!ok) {
        ESP_LOGW("draw", "JPEG %dx%d not decoded", W, H);
        return false;
    }
    if (px == out) {
        return true;
    }
    /* The middle square, nearest pixel. */
    const int s = w < h ? w : h, ox = (w - s) / 2, oy = (h - s) / 2;
    for (int y = 0; y < side; y++) {
        const uint16_t *row = px + (size_t)(oy + y * s / side) * w + ox;
        for (int x = 0; x < side; x++) {
            out[y * side + x] = row[x * s / side];
        }
    }
    heap_caps_free(px);
    return true;
}
