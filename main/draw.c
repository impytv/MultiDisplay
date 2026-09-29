/* Drawing into a custom draw callback's layer (the radar, ship and rain
 * screens and the departure board paint themselves this way rather than
 * with a widget per element). LVGL renders in horizontal strips, calling
 * the callback once per strip, so every primitive is culled against the
 * strip first: a draw task per element per strip would otherwise cost a lot
 * of transient internal DRAM. Text is in the body font. */

#include <string.h>

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

/* Draw `txt` left-aligned in w px, shortened with ".." if it doesn't fit. */
void draw_text_fit(lv_layer_t *layer, const char *txt, int x, int y, int w, lv_color_t color)
{
    if (!draw_visible(layer, x, y, x + w - 1, y + lv_font_get_line_height(g_font_body) - 1)) {
        return;
    }
    char buf[128]; /* longer than anything drawn: a disruption notice */
    const size_t full = strlen(txt);
    size_t len = full;
    if (len > sizeof(buf) - 3) {
        len = sizeof(buf) - 3;
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
        lv_text_get_size(&sz, buf, g_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= w || n <= 1) {
            break;
        }
    }
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
