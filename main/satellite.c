/* The satellite screen: MET Norway's latest Meteosat image of Europe
 * (components/sat_client.c) - infrared, which works day and night, or if
 * so set, visible light while it is light everywhere shown.
 *
 *  - Europe: the whole 1280 x 720 image shrunk to 800 x 450, under the
 *    title. It carries its own logos and time.
 *  - Close-up: the part of it around a location blown up two or three
 *    times (g_cfg->sat_zoom; an image pixel is about 8 km, so that is about
 *    3200 or 2000 km across), with a ring on the location. The image isn't
 *    sharp enough for more.
 *
 * One image serves all of them: each fetch is decoded as it streams in,
 * straight into the views in use, so the ~1 MB file is never held. That
 * is also why the choice of infrared or visible is one for all views - two
 * kinds held at once would mean a full fetch at every switch between
 * them. A new image comes every 15 minutes; it is fetched when MET's
 * Expires says so (or the light changes), while one of these screens is
 * on show. */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app.h"
#include "diag.h"
#include "draw.h"
#include "sat_client.h"
#include "satellite.h"
#include "sun.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "satellite";

#define SAT_X         12
#define SAT_TOP       30  /* the image, under the title row */
#define SAT_VIEW_W    800
#define SAT_VIEW_H    450
#define SAT_SUN_DEG   5.0 /* the visible image is used with the sun at least this high */
#define SAT_RING_R    9
#define SAT_POLL_MS   (15 * 60 * 1000) /* at most, between asking */
#define SAT_MIN_MS    60000            /* at least, after an image */
#define SAT_RETRY_MS  60000
#define SAT_OLD_S     (60 * 60)        /* an older image's time is shown in orange */
#define SAT_STALE     0xE07000

static lv_obj_t *s_root, *s_canvas, *s_title, *s_time;

/* The views in use, RGB565 in PSRAM: Europe if it is shown, and a close-up
 * per location that has one (NULL where not, or if it isn't on the image). */
static uint16_t *s_eu_px;
static lv_image_dsc_t s_eu_img;
static uint16_t *s_loc_px[APP_CONFIG_MAX_LOCATIONS];
static lv_image_dsc_t s_loc_img[APP_CONFIG_MAX_LOCATIONS];
static int s_loc_x0[APP_CONFIG_MAX_LOCATIONS], s_loc_y0[APP_CONFIG_MAX_LOCATIONS];
static int s_lw, s_lh; /* image pixels in a close-up: blown up, they fill the view */

/* Places that stand for Europe's land in the image, west and east: the
 * visible image is only used for Europe while the sun is up at both. */
static const struct {
    double lat, lon;
} EUROPE_LIGHT[] = { { 53.0, -8.0 }, { 45.0, 30.0 } };

static sat_shrink_t s_shrink;
static http_cache_t s_cache;
static bool s_visible;   /* the image held is the visible-light one */
static time_t s_taken;   /* when the image held was taken */
static bool s_valid;     /* the views hold a whole image */
static bool s_failed;    /* the last fetch failed while an image is shown */
static int s_loc = -1;   /* on show: a location, or -1 for Europe */

static bool loc_wanted(int i)
{
    return i < g_cfg->location_count && (g_cfg->show[i] & APP_SHOW_SAT);
}

static bool loc_on_image(int i)
{
    double lat = atof(g_cfg->locations[i].lat), lon = atof(g_cfg->locations[i].lon);
    return sat_crop(lat, lon, s_lw, s_lh, &s_loc_x0[i], &s_loc_y0[i]);
}

/* A row of the image as it streams in: into each view in use. */
static void sat_row(int y, const uint8_t *rgb, void *ctx)
{
    (void)ctx;
    if (s_eu_px != NULL) {
        sat_shrink_row(&s_shrink, y, rgb);
    }
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        if (s_loc_px[i] == NULL || y < s_loc_y0[i] || y >= s_loc_y0[i] + s_lh) {
            continue;
        }
        uint16_t *o = s_loc_px[i] + (size_t)(y - s_loc_y0[i]) * s_lw;
        const uint8_t *p = rgb + 3 * s_loc_x0[i];
        for (int x = 0; x < s_lw; x++, p += 3) {
            o[x] = sat_rgb565(p[0], p[1], p[2]);
        }
    }
}

static void satellite_draw_cb(lv_event_t *e)
{
    if (!s_valid) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    if (s_loc < 0) {
        if (s_eu_px == NULL) {
            return;
        }
        d.src = &s_eu_img;
        lv_area_t a = { 0, SAT_TOP, SAT_VIEW_W - 1, SAT_TOP + SAT_VIEW_H - 1 };
        lv_draw_image(layer, &d, &a);
        return;
    }
    if (s_loc_px[s_loc] == NULL) {
        return;
    }
    /* Blown up from the top-left corner to fill the view. */
    d.src = &s_loc_img[s_loc];
    d.scale_x = LV_SCALE_NONE * SAT_VIEW_W / s_lw;
    d.scale_y = LV_SCALE_NONE * SAT_VIEW_H / s_lh;
    d.pivot.x = d.pivot.y = 0;
    d.antialias = 1;
    lv_area_t a = { 0, SAT_TOP, s_lw - 1, SAT_TOP + s_lh - 1 };
    lv_draw_image(layer, &d, &a);

    /* The location: a red ring, edged in black to show on white cloud. */
    float x, y;
    sat_project(atof(g_cfg->locations[s_loc].lat), atof(g_cfg->locations[s_loc].lon), &x, &y);
    const int cx = (int)lroundf((x - s_loc_x0[s_loc]) * SAT_VIEW_W / s_lw);
    const int cy = SAT_TOP + (int)lroundf((y - s_loc_y0[s_loc]) * SAT_VIEW_H / s_lh);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_opa = LV_OPA_TRANSP;
    r.border_width = 3;
    r.border_color = lv_color_hex(0xFF2D2D);
    r.outline_width = 1;
    r.outline_color = lv_color_black();
    r.outline_opa = LV_OPA_COVER;
    lv_area_t ra = { cx - SAT_RING_R, cy - SAT_RING_R, cx + SAT_RING_R, cy + SAT_RING_R };
    lv_draw_rect(layer, &r, &ra);

    /* The logos on the image are outside the close-up: credit it here. */
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = &lv_font_montserrat_14;
    l.color = lv_color_white();
    l.text = "EUMETSAT / MET Norge";
    l.align = LV_TEXT_ALIGN_RIGHT;
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa = LV_OPA_50;
    lv_point_t sz;
    lv_text_get_size(&sz, l.text, l.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int tw = sz.x + 12;
    lv_area_t ba = { SAT_VIEW_W - tw, EXAMPLE_LCD_V_RES - 20, SAT_VIEW_W - 1, EXAMPLE_LCD_V_RES - 1 };
    lv_draw_rect(layer, &bg, &ba);
    lv_area_t ta = { ba.x1, ba.y1 + 2, ba.x2 - 6, ba.y2 };
    lv_draw_label(layer, &l, &ta);
}

/* Labels for the view on show (adapter lock held). */
static void satellite_show(void)
{
    const bool eu = s_loc < 0;
    if (eu) {
        lv_label_set_text(s_title, "Satellittbilde over Europa");
    } else {
        lv_label_set_text_fmt(s_title, "Satellittbilde over %s", g_cfg->locations[s_loc].name);
    }
    const bool have_view = eu ? s_eu_px != NULL : s_loc_px[s_loc] != NULL;
    if (s_valid && have_view && s_taken > 0) {
        struct tm lt;
        localtime_r(&s_taken, &lt);
        if (g_cfg->sat_visible) {
            lv_label_set_text_fmt(s_time, "%s kl. %02d:%02d", s_visible ? "Synlig lys" : "Infrar\xC3\xB8""dt",
                                  lt.tm_hour, lt.tm_min);
        } else {
            lv_label_set_text_fmt(s_time, "Bilde kl. %02d:%02d", lt.tm_hour, lt.tm_min);
        }
    } else {
        lv_label_set_text(s_time, "");
    }
    if (s_failed || time(NULL) - s_taken > SAT_OLD_S) {
        lv_obj_set_style_text_color(s_time, lv_color_hex(SAT_STALE), 0);
    } else {
        lv_obj_remove_local_style_prop(s_time, LV_STYLE_TEXT_COLOR, 0);
    }
    if (!eu && !have_view && !loc_on_image(s_loc)) {
        lv_label_set_text_fmt(g_status_label, "%s er utenfor satellittbildet.", g_cfg->locations[s_loc].name);
    } else if (!have_view) {
        lv_label_set_text(g_status_label, "Ikke nok minne til satellittbildet.");
    } else if (!s_valid) {
        lv_label_set_text(g_status_label, "Henter satellittbildet...");
    } else {
        lv_label_set_text(g_status_label, "");
    }
    lv_obj_invalidate(s_canvas);
}

static void img_init(lv_image_dsc_t *img, const uint16_t *px, int w, int h)
{
    *img = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                    .w = w, .h = h, .stride = w * 2 },
        .data = (const uint8_t *)px,
        .data_size = (uint32_t)w * h * 2,
    };
}

lv_obj_t *satellite_build(lv_obj_t *screen)
{
    s_root = screen_root_create(screen);
    s_lw = (SAT_VIEW_W + g_cfg->sat_zoom / 2) / g_cfg->sat_zoom; /* 267 or 400 */
    s_lh = SAT_VIEW_H / g_cfg->sat_zoom;                         /* 150 or 225 */
    bool no_mem = false;
    if (g_cfg->sat_show) {
        s_eu_px = heap_caps_calloc((size_t)SAT_VIEW_W * SAT_VIEW_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        no_mem |= s_eu_px == NULL;
        img_init(&s_eu_img, s_eu_px, SAT_VIEW_W, SAT_VIEW_H);
    }
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        if (!loc_wanted(i)) {
            continue;
        }
        if (!loc_on_image(i)) {
            ESP_LOGW(TAG, "%s isn't on the satellite image", g_cfg->locations[i].name);
            continue;
        }
        s_loc_px[i] = heap_caps_calloc((size_t)s_lw * s_lh, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        no_mem |= s_loc_px[i] == NULL;
        img_init(&s_loc_img[i], s_loc_px[i], s_lw, s_lh);
    }
    if (no_mem) {
        ESP_LOGE(TAG, "Out of memory for the satellite views");
    }

    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, satellite_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, SAT_X, 2);
    lv_obj_set_width(s_title, 560);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);

    s_time = lv_label_create(s_root);
    lv_obj_align(s_time, LV_ALIGN_TOP_RIGHT, -SAT_X, 4);
    lv_label_set_text(s_time, "");
    return s_root;
}

/* Whether to fetch the visible-light image at `now`: if so set, and the
 * sun is up at every place shown - Europe's land, and each close-up. */
static bool want_visible(time_t now)
{
    if (!g_cfg->sat_visible) {
        return false;
    }
    if (s_eu_px != NULL) {
        for (size_t k = 0; k < sizeof(EUROPE_LIGHT) / sizeof(EUROPE_LIGHT[0]); k++) {
            if (sun_elevation_deg(EUROPE_LIGHT[k].lat, EUROPE_LIGHT[k].lon, now) < SAT_SUN_DEG) {
                return false;
            }
        }
    }
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        if (s_loc_px[i] != NULL &&
            sun_elevation_deg(atof(g_cfg->locations[i].lat), atof(g_cfg->locations[i].lon), now) < SAT_SUN_DEG) {
            return false;
        }
    }
    return true;
}

void satellite_enter(int loc)
{
    s_loc = loc;
    s_failed = false;
    satellite_show();
}

uint32_t satellite_poll(int loc, int for_view)
{
    (void)loc;
    const time_t now = time(NULL);
    bool any = s_eu_px != NULL;
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        any |= s_loc_px[i] != NULL;
    }
    if (!any) {
        return SAT_POLL_MS; /* nothing to show it in */
    }
    if (now <= PLAUSIBLE_EPOCH_S) {
        return SAT_RETRY_MS; /* MET's Expires needs the clock */
    }
    const bool visible = want_visible(now);
    if (!s_valid || visible != s_visible) {
        memset(&s_cache, 0, sizeof(s_cache)); /* not the image wanted: ask for the whole of it */
    }

    bool failed = false;
    time_t taken = 0;
    int rows = 0;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (s_eu_px == NULL || sat_shrink_init(&s_shrink, SAT_IMG_W, SAT_IMG_H, SAT_VIEW_W, SAT_VIEW_H, s_eu_px)) {
        err = sat_client_fetch(visible, &s_cache, sat_row, NULL, &taken, &rows);
        sat_shrink_free(&s_shrink);
    }
    if (err == ESP_OK) {
        diag_ok(DIAG_SATELLITE);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            s_valid = true;
            s_visible = visible;
            s_taken = taken ? taken : now;
            esp_lv_adapter_unlock();
        }
        ESP_LOGI(TAG, "New %s image, taken %lld", visible ? "visible" : "infrared", (long long)s_taken);
    } else if (err != HTTP_NOT_MODIFIED) {
        diag_fail(DIAG_SATELLITE, err);
        failed = true;
        if (rows > 0 && esp_lv_adapter_lock(-1) == ESP_OK) {
            s_valid = false; /* part old, part new: not worth showing */
            esp_lv_adapter_unlock();
        }
    }

    if (lock_for_view(for_view)) {
        s_failed = failed && s_valid;
        satellite_show();
        if (failed && !s_valid) {
            lv_label_set_text(g_status_label, "Kunne ikke hente satellittbildet. Pr\xC3\xB8ver igjen...");
        }
        esp_lv_adapter_unlock();
    }
    if (failed || !s_valid) {
        return SAT_RETRY_MS;
    }
    /* Until MET's Expires, within limits. */
    const int64_t left_ms = s_cache.expires > now ? (int64_t)(s_cache.expires - now) * 1000 : 0;
    return (uint32_t)(left_ms < SAT_MIN_MS ? SAT_MIN_MS : (left_ms > SAT_POLL_MS ? SAT_POLL_MS : left_ms));
}
