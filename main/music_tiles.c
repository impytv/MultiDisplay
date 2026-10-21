/* The "Velg musikk" grid (see music_tiles.h). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app.h"
#include "draw.h"
#include "http_util.h"
#include "music_tiles.h"
#include "watchdog.h"
#include "sonos_client.h"
#include "waveshare_rgb_lcd_port.h"
#include "yr_client.h"

static const char *TAG = "music_tiles";

#define MT_X         12
#define MT_IMAGE_MAX (256 * 1024) /* a speaker's 640 px cover is ~90 KB */
#define MT_TIMEOUT_MS 5000
#define MT_RING      4 /* px around the tile playing */
#define MT_TAB_PAD   16
#define MT_TAB_GAP   8
#define MT_LIST_GAP  8

void mgrid_init(mgrid_t *g, lv_obj_t *canvas, int top)
{
    memset(g, 0, sizeof(*g));
    g->canvas = canvas;
    g->top = top;
    for (int i = 0; i < MT_MAX; i++) {
        g->t[i].img = (lv_image_dsc_t){
            .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = MT_TILE, .h = MT_TILE,
                        .stride = MT_TILE * 2 },
            .data_size = MT_TILE * MT_TILE * 2,
        };
    }
}

void mgrid_box(const mgrid_t *g, int i, lv_area_t *a)
{
    const int gap = (BOARD_LCD_H_RES - 2 * MT_X - MT_COLS * MT_TILE) / (MT_COLS - 1);
    const int name_h = 2 * lv_font_get_line_height(g_font_body); /* two lines */
    a->x1 = MT_X + (i % MT_COLS) * (MT_TILE + gap);
    a->y1 = g->top + (i / MT_COLS) * (MT_TILE + 6 + name_h + 6);
    a->x2 = a->x1 + MT_TILE - 1;
    a->y2 = a->y1 + MT_TILE + 6 + name_h - 1;
}

int mgrid_hit(const mgrid_t *g, int x, int y)
{
    for (int i = 0; i < g->count; i++) {
        lv_area_t a;
        mgrid_box(g, i, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
            return i;
        }
    }
    return -1;
}

/* Whether each word of `txt` (split at spaces and hyphens) fits `w` px in
 * `font`. */
static bool words_fit(const char *txt, const lv_font_t *font, int w)
{
    for (const char *p = txt; *p;) {
        const size_t n = strcspn(p, " -");
        char word[24];
        snprintf(word, sizeof(word), "%.*s", (int)(n + (p[n] == '-')), p);
        lv_point_t sz;
        lv_text_get_size(&sz, word, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > w) {
            return false;
        }
        p += n;
        p += *p != '\0';
    }
    return true;
}

void mgrid_draw_word(lv_layer_t *layer, const char *txt, int x, int y, int side, lv_color_t bg)
{
    if (!draw_visible(layer, x, y, x + side - 1, y + side - 1)) {
        return; /* the fonts not measured for nothing */
    }
    draw_rect(layer, x, y, x + side - 1, y + side - 1, 8, bg);
    const int w = side - 8;
    const lv_font_t *const fonts[] = { g_font_large, g_font_body, &lv_font_montserrat_16 };
    const lv_font_t *font = fonts[2];
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]) - 1; i++) {
        if (words_fit(txt, fonts[i], w)) {
            font = fonts[i];
            break;
        }
    }
    lv_point_t sz;
    lv_text_get_size(&sz, txt, font, 0, 0, w, LV_TEXT_FLAG_NONE);
    const int h = sz.y < side ? sz.y : side;
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font;
    d.color = lv_color_white();
    d.text = txt;
    d.text_local = 1;
    d.align = LV_TEXT_ALIGN_CENTER;
    const int ty = y + (side - h) / 2;
    lv_area_t box = { x + 4, ty, x + 4 + w - 1, ty + h - 1 };
    lv_draw_label(layer, &d, &box);
}

void mgrid_draw(lv_layer_t *layer, const mgrid_t *g, lv_color_t card, lv_color_t bg, lv_color_t txt,
                lv_color_t label_bg, lv_color_t mark)
{
    for (int i = 0; i < g->count; i++) {
        const mgrid_tile_t *t = &g->t[i];
        lv_area_t a;
        mgrid_box(g, i, &a);
        if (!draw_visible(layer, a.x1 - MT_RING, a.y1 - MT_RING, a.x2 + MT_RING, a.y2)) {
            continue;
        }
        if (t->on) { /* what plays: a ring around it */
            draw_rect(layer, a.x1 - MT_RING, a.y1 - MT_RING, a.x1 + MT_TILE - 1 + MT_RING,
                      a.y1 + MT_TILE - 1 + MT_RING, 10, mark);
        }
        if (t->ok) {
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = &t->img;
            lv_area_t ia = { a.x1, a.y1, a.x1 + MT_TILE - 1, a.y1 + MT_TILE - 1 };
            lv_draw_image(layer, &d, &ia);
        } else if (t->label[0]) { /* a radio's logo till it comes, or without one */
            mgrid_draw_word(layer, t->label, a.x1, a.y1, MT_TILE, label_bg);
        } else {
            draw_rect(layer, a.x1, a.y1, a.x1 + MT_TILE - 1, a.y1 + MT_TILE - 1, 8, card);
        }
        if (t->dot) { /* from Spotify: its green in a corner */
            draw_dot(layer, a.x1 + MT_TILE - 13, a.y1 + 13, 9, bg);
            draw_dot(layer, a.x1 + MT_TILE - 13, a.y1 + 13, 7, lv_color_hex(0x1DB954));
        }
        draw_text_wrap(layer, t->title[0] ? t->title : "Spilleliste", a.x1, a.y1 + MT_TILE + 6, MT_TILE, 2, txt);
    }
}

uint32_t mgrid_art_hash(const char *art)
{
    /* FNV-1a: a cover's address is up to a few hundred characters, kept
     * per tile only to tell whether the picture is still the one wanted. */
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)art; *p; p++) {
        h = (h ^ *p) * 16777619u;
    }
    return art[0] == '\0' ? 0 : h ? h : 1;
}

mgrid_tile_t *mgrid_items(void)
{
    return heap_caps_calloc(MT_MAX, sizeof(mgrid_tile_t), MALLOC_CAP_SPIRAM);
}

static void free_tile(uint16_t *px);

void mgrid_set(mgrid_t *g, const mgrid_tile_t *items, int n)
{
    /* The pixel buffers: one per tile, moved along with their picture. */
    uint16_t *px[MT_MAX];
    uint32_t px_art[MT_MAX];
    bool ok[MT_MAX];
    for (int i = 0; i < MT_MAX; i++) {
        px[i] = g->t[i].px;
        px_art[i] = g->t[i].px_art;
        ok[i] = g->t[i].ok;
    }
    n = n < 0 ? 0 : n > MT_MAX ? MT_MAX : n;
    for (int i = 0; i < MT_MAX; i++) {
        mgrid_tile_t *t = &g->t[i];
        if (i < n) {
            t->idx = items[i].idx;
            snprintf(t->uri, sizeof(t->uri), "%s", items[i].uri);
            snprintf(t->title, sizeof(t->title), "%s", items[i].title);
            snprintf(t->label, sizeof(t->label), "%s", items[i].label);
            t->dot = items[i].dot;
            snprintf(t->art, sizeof(t->art), "%s", items[i].art);
        } else {
            t->art[0] = '\0';
        }
        t->px = NULL;
        t->px_art = 0;
        t->ok = false;
        t->on = false;
    }
    for (int i = 0; i < n; i++) {
        const uint32_t want = mgrid_art_hash(g->t[i].art);
        for (int k = 0; k < MT_MAX && want; k++) {
            if (px[k] && px_art[k] == want) {
                g->t[i].px = px[k];
                g->t[i].px_art = want;
                g->t[i].ok = ok[k];
                px[k] = NULL;
                break;
            }
        }
    }
    for (int i = 0, k = 0; i < MT_MAX; i++) {
        if (g->t[i].px == NULL && i < n && g->t[i].art[0]) {
            /* An unused buffer, if any, for the next picture. */
            while (k < MT_MAX && px[k] == NULL) {
                k++;
            }
            if (k < MT_MAX) {
                g->t[i].px = px[k];
                px[k++] = NULL;
            }
        }
        g->t[i].img.data = (const uint8_t *)g->t[i].px;
    }
    for (int k = 0; k < MT_MAX; k++) {
        free_tile(px[k]); /* any a shorter list doesn't need */
    }
    g->count = n;
    lv_obj_invalidate(g->canvas);
}

#define TILE_BYTES ((size_t)MT_TILE * MT_TILE * 2)

static void free_tile(uint16_t *px)
{
    if (px) {
        heap_caps_free(px);
        wd_psram_held(-(int)TILE_BYTES);
    }
}

/* A sheet of logos (sonos_logo_find's format), fetched once for all the
 * tiles taken from it in one mgrid_fetch. */
typedef struct {
    http_buf_t buf;
    char url[MT_URL_MAX]; /* "" = none yet; set even if it failed */
    bool ok;
} sheet_t;

/* Room for a tile's picture in *px, if it has none. */
static bool tile_alloc(uint16_t **px)
{
    if (*px == NULL) {
        *px = heap_caps_aligned_alloc(16, TILE_BYTES, MALLOC_CAP_SPIRAM);
        if (*px == NULL) {
            ESP_LOGW(TAG, "No memory for a cover");
            return false;
        }
        wd_psram_held(TILE_BYTES); /* the tiles' while the screen shows */
    }
    return true;
}

/* A cover into px (allocated if NULL, only once there is a picture to
 * decode); whether it came. "SHEET#KEY" is logo KEY in the sheet at SHEET. */
static bool fetch_picture(const char *url, uint16_t **px, sheet_t *sheet)
{
    const char *hash = strchr(url, '#');
    if (hash) {
        const size_t n = (size_t)(hash - url);
        if (strncmp(sheet->url, url, n) != 0 || sheet->url[n] != '\0') {
            http_buf_free(&sheet->buf);
            sheet->buf = (http_buf_t){ .max = MT_IMAGE_MAX };
            snprintf(sheet->url, sizeof(sheet->url), "%.*s", (int)n, url);
            sheet->ok = http_get_buf(sheet->url, yr_client_user_agent(), MT_TIMEOUT_MS,
                                     strncmp(url, "https://", 8) == 0, &sheet->buf, TAG) == ESP_OK &&
                        sheet->buf.len > 0;
        }
        const uint8_t *pic;
        size_t len;
        const bool ok = sheet->ok &&
                        sonos_logo_find((const uint8_t *)sheet->buf.buf, sheet->buf.len, hash + 1, &pic, &len) &&
                        tile_alloc(px) && draw_picture_square(pic, len, *px, MT_TILE);
        if (!ok) {
            ESP_LOGW(TAG, "Logo not shown: %.160s", url); /* not asked for again */
        }
        return ok;
    }
    http_buf_t b = { .max = MT_IMAGE_MAX };
    const bool https = strncmp(url, "https://", 8) == 0;
    bool ok = http_get_buf(url, yr_client_user_agent(), MT_TIMEOUT_MS, https, &b, TAG) == ESP_OK &&
              b.buf != NULL && tile_alloc(px) && draw_picture_square((const uint8_t *)b.buf, b.len, *px, MT_TILE);
    http_buf_free(&b);
    if (!ok) {
        ESP_LOGW(TAG, "Cover not shown: %.160s", url); /* not asked for again */
    }
    return ok;
}

bool mgrid_fetch(mgrid_t *g, int max)
{
    static EXT_RAM_BSS_ATTR sheet_t sheet;
    sheet.url[0] = '\0';
    sheet.ok = false;
    for (int i = 0; i < MT_MAX && max > 0; i++) {
        static EXT_RAM_BSS_ATTR char art[MT_URL_MAX];
        uint32_t art_hash = 0;
        uint16_t *px = NULL;
        bool want = false;
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return false;
        }
        mgrid_tile_t *t = &g->t[i];
        if (i < g->count && t->art[0] && (art_hash = mgrid_art_hash(t->art)) != t->px_art) {
            want = true;
            snprintf(art, sizeof(art), "%s", t->art);
            px = t->px;
            t->px = NULL; /* the weather task's while it decodes */
            t->img.data = NULL;
            t->ok = false;
        }
        esp_lv_adapter_unlock();
        if (!want) {
            continue;
        }
        max--;
        const bool ok = fetch_picture(art, &px, &sheet);
        if (!ok) {
            free_tile(px); /* its name shown instead: no memory held for it */
            px = NULL;
        }
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            free_tile(px);
            http_buf_free(&sheet.buf);
            return false;
        }
        if (t->px == NULL) {
            t->px = px;
            t->img.data = (const uint8_t *)px;
            /* Not tried again if it failed (till the list or the screen
             * changes); the tile may hold another item by now, which is
             * then fetched next. */
            t->px_art = art_hash;
            t->ok = ok && strcmp(t->art, art) == 0;
        } else {
            free_tile(px); /* given one meanwhile (mgrid_set) */
        }
        lv_obj_invalidate(g->canvas);
        esp_lv_adapter_unlock();
    }
    http_buf_free(&sheet.buf);
    bool more = false;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        for (int i = 0; i < g->count && !more; i++) {
            more = g->t[i].art[0] && mgrid_art_hash(g->t[i].art) != g->t[i].px_art;
        }
        esp_lv_adapter_unlock();
    }
    return more;
}

void mgrid_release(mgrid_t *g)
{
    /* The pointers are only set by the weather task and under the lock. */
    bool any = false;
    for (int i = 0; i < MT_MAX; i++) {
        any |= g->t[i].px != NULL || g->t[i].px_art != 0; /* a failed one is tried again */
    }
    if (!any || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    for (int i = 0; i < MT_MAX; i++) {
        free_tile(g->t[i].px);
        g->t[i].px = NULL;
        g->t[i].img.data = NULL;
        g->t[i].ok = false;
        g->t[i].px_art = 0;
    }
    esp_lv_adapter_unlock();
}

/* --------------------------------------------------------------------------
 * A tile's picture, kept
 * ------------------------------------------------------------------------ */

bool mpic_take(mpic_t *p, const mgrid_tile_t *t)
{
    mpic_free(p);
    snprintf(p->label, sizeof(p->label), "%s", t->label);
    if (!t->ok || t->px == NULL) {
        return false;
    }
    p->px = heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM);
    if (p->px == NULL) {
        return false;
    }
    memcpy(p->px, t->px, TILE_BYTES);
    p->img = t->img;
    p->img.data = (const uint8_t *)p->px;
    return true;
}

void mpic_free(mpic_t *p)
{
    heap_caps_free(p->px);
    p->px = NULL;
    p->label[0] = '\0';
}

void mpic_draw(lv_layer_t *layer, const mpic_t *p, int x, int y, int side, lv_color_t card, lv_color_t label_bg)
{
    if (!draw_visible(layer, x, y, x + side - 1, y + side - 1)) {
        return;
    }
    if (p->px) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &p->img;
        d.scale_x = d.scale_y = LV_SCALE_NONE * side / MT_TILE;
        d.pivot.x = d.pivot.y = 0;
        d.antialias = 1;
        lv_area_t a = { x, y, x + MT_TILE - 1, y + MT_TILE - 1 };
        lv_draw_image(layer, &d, &a);
    } else if (p->label[0]) {
        mgrid_draw_word(layer, p->label, x, y, side, label_bg);
    } else {
        draw_rect(layer, x, y, x + side - 1, y + side - 1, 8, card);
    }
}

/* --------------------------------------------------------------------------
 * Tabs
 * ------------------------------------------------------------------------ */

void mtabs_box(const char *const *labels, int i, int x, int y, int h, lv_area_t *a)
{
    for (int k = 0; k < i; k++) {
        x += draw_text_w(labels[k]) + 2 * MT_TAB_PAD + MT_TAB_GAP;
    }
    a->x1 = x;
    a->x2 = x + draw_text_w(labels[i]) + 2 * MT_TAB_PAD - 1;
    a->y1 = y;
    a->y2 = y + h - 1;
}

int mtabs_hit(const char *const *labels, int n, int x, int y, int h, int px, int py)
{
    for (int i = 0; i < n; i++) {
        lv_area_t a;
        mtabs_box(labels, i, x, y, h, &a);
        if (px >= a.x1 - MT_TAB_GAP / 2 && px <= a.x2 + MT_TAB_GAP / 2 && py >= a.y1 - 6 && py <= a.y2 + 6) {
            return i;
        }
    }
    return -1;
}

void mtabs_draw(lv_layer_t *layer, const char *const *labels, int n, int sel, int x, int y, int h, lv_color_t fill,
                lv_color_t sel_fill, lv_color_t txt, lv_color_t sel_txt)
{
    const int lh = lv_font_get_line_height(g_font_body);
    for (int i = 0; i < n; i++) {
        lv_area_t a;
        mtabs_box(labels, i, x, y, h, &a);
        draw_rect(layer, a.x1, a.y1, a.x2, a.y2, h / 2, i == sel ? sel_fill : fill);
        draw_text(layer, labels[i], a.x1, a.y1 + (h - lh) / 2, a.x2 - a.x1 + 1, LV_TEXT_ALIGN_CENTER,
                  i == sel ? sel_txt : txt);
    }
}

/* --------------------------------------------------------------------------
 * The queue's list
 * ------------------------------------------------------------------------ */

void mlist_box(int top, int i, lv_area_t *a)
{
    const int lh = lv_font_get_line_height(g_font_body);
    const int bottom = BOARD_LCD_V_RES - lh - 12; /* the note line under it */
    const int h = (bottom - top - (MT_LIST_ROWS - 1) * MT_LIST_GAP) / MT_LIST_ROWS;
    const int w = (BOARD_LCD_H_RES - 2 * MT_X - MT_LIST_GAP) / 2;
    /* Down the first column, then the second, as a list reads. */
    a->x1 = MT_X + (i / MT_LIST_ROWS) * (w + MT_LIST_GAP);
    a->y1 = top + (i % MT_LIST_ROWS) * (h + MT_LIST_GAP);
    a->x2 = a->x1 + w - 1;
    a->y2 = a->y1 + h - 1;
}

int mlist_hit(int top, int n, int x, int y)
{
    for (int i = 0; i < n && i < MT_LIST_MAX; i++) {
        lv_area_t a;
        mlist_box(top, i, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
            return i;
        }
    }
    return -1;
}

void mlist_draw_item(lv_layer_t *layer, int top, int i, const char *title, const char *artist, bool current,
                     lv_color_t card, lv_color_t txt, lv_color_t dim, lv_color_t cur_fill, lv_color_t cur_txt)
{
    lv_area_t a;
    mlist_box(top, i, &a);
    if (!draw_visible(layer, a.x1, a.y1, a.x2, a.y2)) {
        return;
    }
    draw_rect(layer, a.x1, a.y1, a.x2, a.y2, 10, current ? cur_fill : card);
    const int lh = lv_font_get_line_height(g_font_body);
    const int ty = a.y1 + (a.y2 - a.y1 + 1 - (artist[0] ? 2 * lh + 2 : lh)) / 2;
    draw_text_fit(layer, title[0] ? title : "\xE2\x80\x93", a.x1 + 14, ty, a.x2 - a.x1 - 27, current ? cur_txt : txt);
    if (artist[0]) {
        draw_text_fit(layer, artist, a.x1 + 14, ty + lh + 2, a.x2 - a.x1 - 27, current ? cur_txt : dim);
    }
}
