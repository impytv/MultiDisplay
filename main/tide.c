/* The tide screen: Kartverket's water level for a location on the coast
 * (components/tide_client.c).
 *
 *  - Left: the level now, whether it is rising or falling, how much the
 *    weather adds or takes away, and the next high and low tides.
 *  - Right: the level from six hours ago to 30 hours ahead - the forecast
 *    (tide and weather), the tide alone, and what was measured - with high
 *    and low tide marked.
 *
 * Painted by one draw handler like the air screen. Fetched every 30
 * minutes; redrawn every few minutes so "now" moves on. Inland there is no
 * data, and the screen says so. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app.h"
#include "diag.h"
#include "draw.h"
#include "tide.h"
#include "tide_client.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "tide";

#define TIDE_X           12
#define TIDE_TOP         56
#define TIDE_PANEL_W     280
#define TIDE_CHART_X0    (TIDE_X + TIDE_PANEL_W + 56) /* room for the level labels */
#define TIDE_CHART_X1    (EXAMPLE_LCD_H_RES - 14)
#define TIDE_BEFORE_S    (6 * 3600)
#define TIDE_AFTER_S     (30 * 3600)
#define TIDE_FETCH_MS    (30 * 60 * 1000)
#define TIDE_KEEP_MS     (6 * 3600 * 1000) /* older than this isn't shown */
#define TIDE_POLL_MS     (5 * 60 * 1000)
#define TIDE_RETRY_MS    30000
#define TIDE_NEXT        4                 /* high and low tides listed */
#define TIDE_FORECAST    0x2E86DE
#define TIDE_NOW         0xE07000
#define TIDE_STALE       0xE07000

static lv_obj_t *s_root, *s_canvas, *s_title, *s_updated, *s_now;
static int s_loc = -1;

/* Per location, in PSRAM, for locations with a tide screen. */
static tide_t *s_tide[APP_CONFIG_MAX_LOCATIONS];
static fetch_stamp_t s_at[APP_CONFIG_MAX_LOCATIONS];
static tide_t *s_scratch;
static bool s_failed; /* the last poll failed while older data is shown */

static lv_color_t text_colour(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0xDDE6EE : 0x1B2631);
}

static lv_color_t dim_colour(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0x8AA0B4 : 0x5D6D7E);
}

static lv_color_t grid_colour(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0x34495E : 0xD5DBE0);
}

static const tide_t *shown(void)
{
    return (s_loc >= 0 && s_tide[s_loc] != NULL && stamp_fresh(&s_at[s_loc], TIDE_KEEP_MS)) ? s_tide[s_loc] : NULL;
}

/* The level at `when`: the forecast, or the tide alone where there is none. */
static float level_at(const tide_t *t, int64_t when)
{
    const float f = tide_at(t, t->forecast, when);
    return isnan(f) ? tide_at(t, t->prediction, when) : f;
}

/* "17:06" today, "lør 05:36" on another day. */
static const char *const WD[] = { "s\xC3\xB8n", "man", "tir", "ons", "tor", "fre", "l\xC3\xB8r" };

static void when_text(char *dst, size_t len, int64_t t, time_t now)
{
    const time_t tt = (time_t)t;
    struct tm a, b;
    localtime_r(&tt, &a);
    localtime_r(&now, &b);
    if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) {
        snprintf(dst, len, "%02d:%02d", a.tm_hour, a.tm_min);
    } else {
        snprintf(dst, len, "%s %02d:%02d", WD[a.tm_wday], a.tm_hour, a.tm_min);
    }
}

/* The widest when_text() can be at the font size set: the widest weekday
 * with the widest digit in every place ("man 00:00" is wider than
 * "s\xC3\xB8n 88:88"). */
static int when_max_w(void)
{
    const char *day = WD[0];
    for (int i = 1; i < 7; i++) {
        if (draw_text_w(WD[i]) > draw_text_w(day)) {
            day = WD[i];
        }
    }
    char d[2] = "0", widest = '0';
    for (char c = '1'; c <= '9'; c++) {
        d[0] = c;
        const int w = draw_text_w(d);
        d[0] = widest;
        if (w > draw_text_w(d)) {
            widest = c;
        }
    }
    char txt[16];
    snprintf(txt, sizeof(txt), "%s %c%c:%c%c", day, widest, widest, widest, widest);
    return draw_text_w(txt);
}

static void draw_series(lv_layer_t *layer, const tide_t *t, const float *s, int64_t t0, int64_t t1, float lo,
                        float hi, int y0, int y1, int width, lv_color_t c)
{
    const int x0 = TIDE_CHART_X0, x1 = TIDE_CHART_X1;
    int px = 0, py = 0;
    bool have = false;
    for (int k = 0; k < t->count; k++) {
        const int64_t when = t->start + (int64_t)k * TIDE_STEP_S;
        if (when < t0 || when > t1 || isnan(s[k])) {
            have = false;
            continue;
        }
        const int x = x0 + (int)((when - t0) * (x1 - x0) / (t1 - t0));
        const int y = y1 - (int)((s[k] - lo) * (float)(y1 - y0) / (hi - lo));
        if (have) {
            draw_line(layer, px, py, x, y, width, c);
        }
        px = x;
        py = y;
        have = true;
    }
}

static void tide_draw_cb(lv_event_t *e)
{
    const tide_t *t = shown();
    if (t == NULL || t->no_data) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    const int lh = lv_font_get_line_height(g_font_body);
    const int big = lv_font_get_line_height(g_font_large);
    const lv_color_t c_txt = text_colour(), c_dim = dim_colour();
    const time_t now = time(NULL);

    /* Left: now, the weather's part, the next high and low tides. */
    draw_text(layer, "N\xC3\xA5", TIDE_X, TIDE_TOP, 100, LV_TEXT_ALIGN_LEFT, c_dim);
    const float v = level_at(t, now), v2 = level_at(t, now + TIDE_STEP_S);
    int y = TIDE_TOP + lh + big + 6;
    if (!isnan(v) && !isnan(v2)) {
        draw_text(layer, v2 > v + 0.5f ? "stiger" : v2 < v - 0.5f ? "synker" : "snur", TIDE_X, y, TIDE_PANEL_W,
                  LV_TEXT_ALIGN_LEFT, c_txt);
        y += lh + 2;
    }
    const float f = tide_at(t, t->forecast, now), p = tide_at(t, t->prediction, now);
    if (!isnan(f) && !isnan(p)) {
        char line[48];
        snprintf(line, sizeof(line), "V\xC3\xA6r og vind: %+.0f cm", f - p);
        draw_text(layer, line, TIDE_X, y, TIDE_PANEL_W, LV_TEXT_ALIGN_LEFT, c_dim);
    }
    y += lh + 16;
    /* Columns as wide as their widest text at the font size set. */
    const int time_x = TIDE_X + draw_text_w("H\xC3\xB8yvann") + 12;
    const int time_w = when_max_w() + 8;
    int listed = 0;
    for (int i = 0; i < t->extreme_count && listed < TIDE_NEXT; i++) {
        const tide_extreme_t *x = &t->extremes[i];
        if (x->time < now) {
            continue;
        }
        char when[24], cm[16];
        when_text(when, sizeof(when), x->time, now);
        snprintf(cm, sizeof(cm), "%.0f", x->cm); /* the unit is in the footer */
        draw_text(layer, x->high ? "H\xC3\xB8yvann" : "Lavvann", TIDE_X, y, time_x - TIDE_X, LV_TEXT_ALIGN_LEFT,
                  c_txt);
        draw_text(layer, when, time_x, y, time_w, LV_TEXT_ALIGN_LEFT, c_txt);
        draw_text(layer, cm, time_x + time_w, y, TIDE_X + TIDE_PANEL_W - time_x - time_w, LV_TEXT_ALIGN_RIGHT, c_dim);
        y += lh + 6;
        listed++;
    }

    /* Right: the chart. */
    const int64_t t0 = now - TIDE_BEFORE_S, t1 = now + TIDE_AFTER_S;
    float lo = INFINITY, hi = -INFINITY;
    const float *series[] = { t->prediction, t->forecast, t->observation };
    for (int s = 0; s < 3; s++) {
        for (int k = 0; k < t->count; k++) {
            const int64_t when = t->start + (int64_t)k * TIDE_STEP_S;
            if (when >= t0 && when <= t1 && !isnan(series[s][k])) {
                lo = fminf(lo, series[s][k]);
                hi = fmaxf(hi, series[s][k]);
            }
        }
    }
    if (!(hi > lo)) {
        return;
    }
    const float grid = (hi - lo) > 250.0f ? 100.0f : (hi - lo) > 100.0f ? 50.0f : 25.0f;
    lo = floorf((lo - 5.0f) / grid) * grid;
    hi = ceilf((hi + 5.0f) / grid) * grid;
    const int y0 = TIDE_TOP + lh + 14, y1 = EXAMPLE_LCD_V_RES - lh - 14;
    const int x0 = TIDE_CHART_X0, x1 = TIDE_CHART_X1;
    const lv_color_t c_grid = grid_colour();

    /* Above the chart: the station, and what the lines are. */
    char label[64];
    const int gap = 14, w_tide = draw_text_w("tidevann"), w_obs = draw_text_w("m\xC3\xA5lt"),
              w_fc = draw_text_w("varslet");
    const int legend_x = x1 - w_tide - w_obs - w_fc - 2 * gap;
    draw_text(layer, "varslet", legend_x, TIDE_TOP, w_fc + 2, LV_TEXT_ALIGN_LEFT, lv_color_hex(TIDE_FORECAST));
    draw_text(layer, "m\xC3\xA5lt", legend_x + w_fc + gap, TIDE_TOP, w_obs + 2, LV_TEXT_ALIGN_LEFT, c_txt);
    draw_text(layer, "tidevann", x1 - w_tide, TIDE_TOP, w_tide + 2, LV_TEXT_ALIGN_LEFT, c_dim);
    snprintf(label, sizeof(label), "M\xC3\xA5lested %s", t->station[0] ? t->station : "ukjent");
    draw_text_fit(layer, label, x0 - 50, TIDE_TOP, legend_x - gap - (x0 - 50), c_dim);

    for (float g = lo; g <= hi + 0.5f; g += grid) {
        const int gy = y1 - (int)((g - lo) * (float)(y1 - y0) / (hi - lo));
        draw_line(layer, x0, gy, x1, gy, 1, c_grid);
        snprintf(label, sizeof(label), "%.0f", g);
        draw_text(layer, label, x0 - 70, gy - lh / 2, 62, LV_TEXT_ALIGN_RIGHT, c_dim);
    }
    /* Every six hours: a tick and the hour. */
    const int64_t first = (t0 / 3600 + 1) * 3600;
    for (int64_t h = first; h <= t1; h += 3600) {
        const time_t ht = (time_t)h;
        struct tm lt;
        localtime_r(&ht, &lt);
        if (lt.tm_hour % 6 != 0) {
            continue;
        }
        const int hx = x0 + (int)((h - t0) * (x1 - x0) / (t1 - t0));
        draw_line(layer, hx, y0, hx, y1, 1, c_grid);
        snprintf(label, sizeof(label), "%02d", lt.tm_hour);
        draw_text(layer, label, hx - 20, y1 + 4, 40, LV_TEXT_ALIGN_CENTER, c_dim);
    }
    draw_series(layer, t, t->prediction, t0, t1, lo, hi, y0, y1, 1, c_dim);
    draw_series(layer, t, t->forecast, t0, t1, lo, hi, y0, y1, 3, lv_color_hex(TIDE_FORECAST));
    draw_series(layer, t, t->observation, t0, t1, lo, hi, y0, y1, 2, c_txt);
    const int nx = x0 + (int)((now - t0) * (x1 - x0) / (t1 - t0));
    draw_line(layer, nx, y0, nx, y1, 2, lv_color_hex(TIDE_NOW));

    /* High and low tide: a dot and the time. */
    for (int i = 0; i < t->extreme_count; i++) {
        const tide_extreme_t *x = &t->extremes[i];
        if (x->time < t0 || x->time > t1) {
            continue;
        }
        const float lv = level_at(t, x->time);
        const int ex = x0 + (int)((x->time - t0) * (x1 - x0) / (t1 - t0));
        const int ey = y1 - (int)(((isnan(lv) ? x->cm : lv) - lo) * (float)(y1 - y0) / (hi - lo));
        draw_dot(layer, ex, ey, 4, lv_color_hex(TIDE_FORECAST));
        const time_t xt = (time_t)x->time;
        struct tm lt;
        localtime_r(&xt, &lt);
        snprintf(label, sizeof(label), "%02d:%02d", lt.tm_hour, lt.tm_min);
        const int ly = x->high ? ey - lh - 6 : ey + 6;
        draw_text(layer, label, ex - 40, ly < y0 ? y0 : ly > y1 - lh ? y1 - lh : ly, 80, LV_TEXT_ALIGN_CENTER, c_dim);
    }
    draw_text_fit(layer, "cm over sj\xC3\xB8kartnull", TIDE_X, EXAMPLE_LCD_V_RES - 2 * lh - 4, TIDE_PANEL_W, c_dim);
    draw_text_fit(layer, "Kilde: Kartverket", TIDE_X, EXAMPLE_LCD_V_RES - lh - 4, TIDE_PANEL_W, c_dim);
}

/* Labels for what is held for s_loc (adapter lock held). */
static void tide_show(void)
{
    const tide_t *t = shown();
    const float v = (t != NULL && !t->no_data) ? level_at(t, time(NULL)) : NAN;
    if (!isnan(v)) {
        lv_label_set_text_fmt(s_now, "%.0f cm", v);
    } else {
        lv_label_set_text(s_now, "");
    }
    const fetch_stamp_t *at = &s_at[s_loc];
    if (t != NULL && !t->no_data && at->when > 0) {
        struct tm lt;
        localtime_r(&at->when, &lt);
        lv_label_set_text_fmt(s_updated, "%s kl. %02d:%02d", s_failed ? "Sist oppdatert" : "Oppdatert", lt.tm_hour,
                              lt.tm_min);
    } else {
        lv_label_set_text(s_updated, "");
    }
    if (s_failed) {
        lv_obj_set_style_text_color(s_updated, lv_color_hex(TIDE_STALE), 0);
    } else {
        lv_obj_remove_local_style_prop(s_updated, LV_STYLE_TEXT_COLOR, 0);
    }
    if (t == NULL) {
        lv_label_set_text(g_status_label, "Henter vannstand...");
    } else if (t->no_data) {
        lv_label_set_text_fmt(g_status_label, "Ingen vannstand for %s:\nstedet ligger for langt fra kysten.",
                              g_cfg->locations[s_loc].name);
    } else {
        lv_label_set_text(g_status_label, "");
    }
    lv_obj_invalidate(s_canvas);
}

lv_obj_t *tide_build(lv_obj_t *screen)
{
    s_root = screen_root_create(screen);
    s_scratch = heap_caps_malloc(sizeof(*s_scratch), MALLOC_CAP_SPIRAM);
    assert(s_scratch != NULL);
    for (int i = 0; i < g_cfg->location_count; i++) {
        if (g_cfg->show[i] & APP_SHOW_TIDE) {
            s_tide[i] = heap_caps_calloc(1, sizeof(tide_t), MALLOC_CAP_SPIRAM);
        }
    }

    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, tide_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, TIDE_X, 4);
    lv_obj_set_width(s_title, 500);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);

    s_updated = lv_label_create(s_root);
    lv_obj_align(s_updated, LV_ALIGN_TOP_RIGHT, -TIDE_X, 4);
    lv_label_set_text(s_updated, "");

    s_now = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_now, g_font_large, 0);
    lv_obj_set_pos(s_now, TIDE_X, TIDE_TOP + lv_font_get_line_height(g_font_body) + 2);
    lv_label_set_text(s_now, "");
    return s_root;
}

void tide_enter(int loc)
{
    s_loc = loc;
    s_failed = false;
    lv_label_set_text_fmt(s_title, "Tidevann i %s", g_cfg->locations[loc].name);
    tide_show();
}

uint32_t tide_poll(int loc, int for_view)
{
    const time_t now = time(NULL);
    if (s_tide[loc] == NULL || now <= PLAUSIBLE_EPOCH_S) {
        return TIDE_RETRY_MS; /* the request needs the date */
    }
    bool failed = false;
    if (!stamp_fresh(&s_at[loc], TIDE_FETCH_MS)) {
        const double lat = atof(g_cfg->locations[loc].lat), lon = atof(g_cfg->locations[loc].lon);
        esp_err_t err = tide_fetch(lat, lon, now, s_scratch);
        if (err == ESP_OK) {
            diag_ok(DIAG_TIDE);
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                *s_tide[loc] = *s_scratch;
                stamp_now(&s_at[loc]);
                esp_lv_adapter_unlock();
            }
            ESP_LOGI(TAG, "Tide[%d] %s: %s %s, %d high/low", loc, g_cfg->locations[loc].name,
                     s_scratch->no_data ? "no data" : "from", s_scratch->station, s_scratch->extreme_count);
        } else {
            diag_fail(DIAG_TIDE, err);
            failed = true;
        }
    }
    bool ok = false;
    if (lock_for_view(for_view)) {
        s_failed = failed && shown() != NULL;
        tide_show();
        ok = shown() != NULL;
        if (!ok && failed) {
            lv_label_set_text(g_status_label, "Kunne ikke hente vannstand. Pr\xC3\xB8ver igjen...");
        }
        esp_lv_adapter_unlock();
    }
    return ok ? TIDE_POLL_MS : TIDE_RETRY_MS;
}
