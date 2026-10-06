/* The air screen: MET's air quality forecast and a pollen forecast for a
 * location (components/air_client.c).
 *
 *  - Top: the level now in a box in MET's colour for it ("Lite", "Moderat",
 *    ...), the pollutant driving it, and the next 24 hours as one coloured
 *    cell per hour.
 *  - Below: one row per pollen type with its level now in NAAF's words and
 *    the next 24 hours as bars; out of season, one line saying so.
 *
 * Painted by one draw handler like the departure board. Air quality is
 * fetched every 30 minutes (MET's Expires honoured), pollen every hour; the
 * screen is redrawn every few minutes so "now" moves on with the clock. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "air.h"
#include "air_client.h"
#include "app.h"
#include "diag.h"
#include "draw.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "air";

#define AIR_X             12
#define AIR_TOP           56
#define AIR_BOX_W         240
#define AIR_BOX_H         104
#define AIR_CELL_H        34
#define AIR_HOURS_SHOWN   24
#define AIR_QUALITY_MS    (30 * 60 * 1000)
#define AIR_POLLEN_MS     (60 * 60 * 1000)
#define AIR_KEEP_MS       (3 * 3600 * 1000) /* older than this isn't shown */
#define AIR_POLL_MS       (5 * 60 * 1000)
#define AIR_RETRY_MS      30000
#define AIR_STALE_COLOUR  0xE07000

static lv_obj_t *s_root, *s_canvas, *s_title, *s_updated;
static lv_obj_t *s_box, *s_box_level, *s_box_sub;
static int s_loc = -1;

/* Per location, in PSRAM, for locations with an air screen. */
static air_quality_t *s_q[APP_CONFIG_MAX_LOCATIONS];
static air_pollen_t *s_p[APP_CONFIG_MAX_LOCATIONS];
static fetch_stamp_t s_q_at[APP_CONFIG_MAX_LOCATIONS], s_p_at[APP_CONFIG_MAX_LOCATIONS];
static http_cache_t s_q_http[APP_CONFIG_MAX_LOCATIONS];
static air_quality_t *s_q_scratch;
static air_pollen_t *s_p_scratch;
static bool s_failed; /* the last poll failed while older data is shown */

static lv_color_t text_colour(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0xDDE6EE : 0x1B2631);
}

static lv_color_t dim_colour(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0x8AA0B4 : 0x5D6D7E);
}

static const air_quality_t *shown_q(void)
{
    return (s_loc >= 0 && s_q[s_loc] != NULL && stamp_fresh(&s_q_at[s_loc], AIR_KEEP_MS)) ? s_q[s_loc] : NULL;
}

static const air_pollen_t *shown_p(void)
{
    return (s_loc >= 0 && s_p[s_loc] != NULL && stamp_fresh(&s_p_at[s_loc], AIR_KEEP_MS)) ? s_p[s_loc] : NULL;
}

/* "22", "02", ... under every sixth cell. */
static void hour_label(lv_layer_t *layer, int64_t t, int x, int y, int w, lv_color_t c)
{
    const time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    char h[4];
    snprintf(h, sizeof(h), "%02d", lt.tm_hour);
    draw_text(layer, h, x, y, w, LV_TEXT_ALIGN_LEFT, c);
}

static void air_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const int lh = lv_font_get_line_height(g_font_body);
    const lv_color_t c_txt = text_colour(), c_dim = dim_colour();
    const time_t now = time(NULL);
    const air_quality_t *q = shown_q();
    const air_pollen_t *p = shown_p();

    /* Air quality: the next 24 hours right of the box. */
    const int cells_x = AIR_X + AIR_BOX_W + 20, cells_r = BOARD_LCD_H_RES - AIR_X;
    const int cell_w = (cells_r - cells_x) / AIR_HOURS_SHOWN;
    const int cells_y = AIR_TOP + lh + 4;
    if (q != NULL) {
        draw_text(layer, "Neste d\xC3\xB8gn", cells_x, AIR_TOP, 200, LV_TEXT_ALIGN_LEFT, c_dim);
        const int i0 = air_hour_index(q->start, q->count, now);
        for (int k = 0; i0 >= 0 && k < AIR_HOURS_SHOWN && i0 + k < q->count; k++) {
            const int x = cells_x + k * cell_w;
            draw_rect(layer, x, cells_y, x + cell_w - 2, cells_y + AIR_CELL_H - 1, 2,
                      lv_color_hex(air_aqi_level_colour(air_aqi_level(q->aqi[i0 + k]))));
            if (k % 6 == 0) {
                hour_label(layer, q->start + (int64_t)(i0 + k) * 3600, x, cells_y + AIR_CELL_H + 2, 40, c_dim);
            }
        }
        if (i0 >= 0) {
            /* What drives it now. */
            int worst = 0;
            for (int s = 1; s < AIR_POLLUTANTS; s++) {
                if (q->sub[s][i0] > q->sub[worst][i0]) {
                    worst = s;
                }
            }
            char line[64];
            snprintf(line, sizeof(line), "Mest: %s", air_pollutant_name((air_pollutant_t)worst));
            draw_text(layer, line, AIR_X, AIR_TOP + AIR_BOX_H + 6, 520, LV_TEXT_ALIGN_LEFT, c_dim);
        }
    }

    /* Pollen. */
    const int pollen_y = AIR_TOP + AIR_BOX_H + lh + 22;
    if (p == NULL) {
        return;
    }
    draw_text(layer, "Pollen", AIR_X, pollen_y, 120, LV_TEXT_ALIGN_LEFT, c_txt);
    const int i0 = air_hour_index(p->start, p->count, now);
    int season = 0; /* the highest level over the next day */
    for (int t = 0; t < AIR_POLLEN_TYPES; t++) {
        for (int k = 0; i0 >= 0 && k < AIR_HOURS_SHOWN && i0 + k < p->count; k++) {
            const int l = air_pollen_level((air_pollen_type_t)t, p->grains[t][i0 + k]);
            season = l > season ? l : season;
        }
    }
    const int rows_y = pollen_y + lh + 8;
    if (i0 < 0 || season == 0) {
        draw_text(layer, "Ingen pollen i lufta n\xC3\xA5", AIR_X, rows_y, 500, LV_TEXT_ALIGN_LEFT, c_dim);
        return;
    }
    const int foot_y = BOARD_LCD_V_RES - lh - 4;
    const int row_h = (foot_y - 6 - rows_y) / AIR_POLLEN_TYPES;
    const int bars_x = cells_x, bar_w = cell_w;
    const int bar_max = row_h - 16; /* clear of the row above */
    draw_text(layer, "n\xC3\xA5", AIR_X + 130, pollen_y, 60, LV_TEXT_ALIGN_LEFT, c_dim);
    draw_text(layer, "neste d\xC3\xB8gn", bars_x, pollen_y, 200, LV_TEXT_ALIGN_LEFT, c_dim);
    for (int t = 0; t < AIR_POLLEN_TYPES; t++) {
        const int y = rows_y + t * row_h;
        const int now_level = air_pollen_level((air_pollen_type_t)t, p->grains[t][i0]);
        draw_text(layer, air_pollen_name((air_pollen_type_t)t), AIR_X, y + (row_h - lh) / 2, 120,
                  LV_TEXT_ALIGN_LEFT, c_txt);
        draw_dot(layer, AIR_X + 136, y + row_h / 2, 6, lv_color_hex(air_pollen_level_colour(now_level)));
        draw_text(layer, air_pollen_level_name(now_level), AIR_X + 150, y + (row_h - lh) / 2,
                  bars_x - AIR_X - 160, LV_TEXT_ALIGN_LEFT, c_txt);
        const int base = y + row_h - 4;
        draw_line(layer, bars_x, base, cells_r - 2, base, 1, c_dim);
        for (int k = 0; k < AIR_HOURS_SHOWN && i0 + k < p->count; k++) {
            const int l = air_pollen_level((air_pollen_type_t)t, p->grains[t][i0 + k]);
            if (l > 0) {
                const int x = bars_x + k * bar_w;
                draw_rect(layer, x, base - l * bar_max / 4, x + bar_w - 2, base - 1, 1,
                          lv_color_hex(air_pollen_level_colour(l)));
            }
        }
    }
    draw_text(layer, "Pollen: Copernicus (CAMS) via Open-Meteo. Luft: MET Norge.", AIR_X, foot_y, 700,
              LV_TEXT_ALIGN_LEFT, c_dim);
}

/* Labels and box for what is held for s_loc (adapter lock held). */
static void air_show(void)
{
    const air_quality_t *q = shown_q();
    const int i0 = q ? air_hour_index(q->start, q->count, time(NULL)) : -1;
    if (i0 >= 0) {
        const int level = air_aqi_level(q->aqi[i0]);
        lv_obj_set_style_bg_color(s_box, lv_color_hex(air_aqi_level_colour(level)), 0);
        const lv_color_t on_box = level == 1 ? lv_color_hex(0x1B2631) : lv_color_white();
        lv_obj_set_style_text_color(s_box_level, on_box, 0);
        lv_obj_set_style_text_color(s_box_sub, on_box, 0);
        lv_label_set_text(s_box_level, air_aqi_level_name(level));
        lv_obj_clear_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    }
    const fetch_stamp_t *at = &s_q_at[s_loc];
    if (q != NULL && at->when > 0) {
        struct tm lt;
        localtime_r(&at->when, &lt);
        lv_label_set_text_fmt(s_updated, "%s kl. %02d:%02d", s_failed ? "Sist oppdatert" : "Oppdatert",
                              lt.tm_hour, lt.tm_min);
    } else {
        lv_label_set_text(s_updated, "");
    }
    if (s_failed) {
        lv_obj_set_style_text_color(s_updated, lv_color_hex(AIR_STALE_COLOUR), 0);
    } else {
        lv_obj_remove_local_style_prop(s_updated, LV_STYLE_TEXT_COLOR, 0);
    }
    lv_label_set_text(g_status_label, (q != NULL || shown_p() != NULL) ? "" : "Henter luftkvalitet...");
    lv_obj_invalidate(s_canvas);
}

lv_obj_t *air_build(lv_obj_t *screen)
{
    s_root = screen_root_create(screen);
    s_q_scratch = heap_caps_malloc(sizeof(*s_q_scratch), MALLOC_CAP_SPIRAM);
    s_p_scratch = heap_caps_malloc(sizeof(*s_p_scratch), MALLOC_CAP_SPIRAM);
    assert(s_q_scratch != NULL && s_p_scratch != NULL);
    for (int i = 0; i < g_cfg->location_count; i++) {
        if (g_cfg->show[i] & APP_SHOW_AIR) {
            s_q[i] = heap_caps_calloc(1, sizeof(air_quality_t), MALLOC_CAP_SPIRAM);
            s_p[i] = heap_caps_calloc(1, sizeof(air_pollen_t), MALLOC_CAP_SPIRAM);
        }
    }

    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, air_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, AIR_X, 4);
    lv_obj_set_width(s_title, 500);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);

    s_updated = lv_label_create(s_root);
    lv_obj_align(s_updated, LV_ALIGN_TOP_RIGHT, -AIR_X, 4);
    lv_label_set_text(s_updated, "");

    s_box = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_pos(s_box, AIR_X, AIR_TOP);
    lv_obj_set_size(s_box, AIR_BOX_W, AIR_BOX_H);
    lv_obj_set_style_radius(s_box, 10, 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_box_level = lv_label_create(s_box);
    lv_obj_set_style_text_font(s_box_level, g_font_large, 0);
    lv_obj_align(s_box_level, LV_ALIGN_CENTER, 0, -12);
    s_box_sub = lv_label_create(s_box);
    lv_label_set_text(s_box_sub, "luftforurensning");
    lv_obj_align(s_box_sub, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
    return s_root;
}

void air_enter(int loc)
{
    s_loc = loc;
    s_failed = false;
    lv_label_set_text_fmt(s_title, "Luft i %s", g_cfg->locations[loc].name);
    air_show();
}

uint32_t air_poll(int loc, int for_view)
{
    if (s_q[loc] == NULL || s_p[loc] == NULL) {
        return AIR_RETRY_MS;
    }
    const double lat = atof(g_cfg->locations[loc].lat), lon = atof(g_cfg->locations[loc].lon);
    bool failed = false;

    if (!stamp_fresh(&s_q_at[loc], AIR_QUALITY_MS)) {
        if (!s_q_at[loc].valid) {
            memset(&s_q_http[loc], 0, sizeof(s_q_http[loc]));
        }
        esp_err_t err = air_quality_fetch(lat, lon, s_q_scratch, &s_q_http[loc]);
        if (err == ESP_OK || err == HTTP_NOT_MODIFIED) {
            diag_ok(DIAG_AIR);
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                if (err == ESP_OK) {
                    *s_q[loc] = *s_q_scratch;
                }
                stamp_now(&s_q_at[loc]);
                esp_lv_adapter_unlock();
            }
            ESP_LOGI(TAG, "Air quality[%d] %s: %s", loc, g_cfg->locations[loc].name,
                     err == ESP_OK ? "fetched" : "unchanged");
        } else {
            diag_fail(DIAG_AIR, err);
            failed = true;
        }
    }
    if (!stamp_fresh(&s_p_at[loc], AIR_POLLEN_MS)) {
        esp_err_t err = air_pollen_fetch(lat, lon, s_p_scratch);
        if (err == ESP_OK) {
            diag_ok(DIAG_POLLEN);
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                *s_p[loc] = *s_p_scratch;
                stamp_now(&s_p_at[loc]);
                esp_lv_adapter_unlock();
            }
        } else {
            diag_fail(DIAG_POLLEN, err);
            failed = true;
        }
    }

    bool shown = false;
    if (lock_for_view(for_view)) {
        s_failed = failed && (shown_q() != NULL || shown_p() != NULL);
        air_show();
        shown = shown_q() != NULL;
        if (!shown && failed) {
            lv_label_set_text(g_status_label, "Kunne ikke hente luftkvalitet. Pr\xC3\xB8ver igjen...");
        }
        esp_lv_adapter_unlock();
    }
    return shown ? AIR_POLL_MS : AIR_RETRY_MS;
}
