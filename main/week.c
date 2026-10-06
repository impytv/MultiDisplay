/* The week screen: seven day cards from the location's forecast (the days
 * yr_client sums up): the weekday, MET's symbol for the day, the day's
 * temperature range as a bar on a scale shared by the whole week, with the
 * high above and the low below it, the precipitation total and the
 * strongest wind. Uses the weather screen's forecast cache (weather.c). */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "draw.h"
#include "waveshare_rgb_lcd_port.h"
#include "weather.h"
#include "week.h"

#define WEEK_DAYS      7
#define WEEK_X         12
#define WEEK_TOP       54
#define WEEK_GAP       6
#define WEEK_ICON      64
#define WEEK_BAR_W     14
#define WEEK_POLL_MS   (10 * 60 * 1000)
#define WEEK_RETRY_MS  20000

static lv_obj_t *s_root, *s_canvas, *s_title, *s_updated;
static lv_obj_t *s_icon[WEEK_DAYS];
static int s_loc = -1;

static bool dark(void)
{
    return g_cfg->theme == APP_THEME_DARK;
}

static int card_w(void)
{
    return (BOARD_LCD_H_RES - 2 * WEEK_X - (WEEK_DAYS - 1) * WEEK_GAP) / WEEK_DAYS;
}

/* Where things go in a card, from the body font's line height. */
typedef struct {
    int day, date, icon, bar_top, bar_bot, precip, wind;
} week_rows_t;

static week_rows_t rows(void)
{
    const int lh = lv_font_get_line_height(g_font_body);
    week_rows_t r;
    r.day = WEEK_TOP + 6;
    r.date = r.day + lh;
    r.icon = r.date + lh + 4;
    r.wind = BOARD_LCD_V_RES - lh - 12;
    r.precip = r.wind - lh - 2;
    r.bar_top = r.icon + WEEK_ICON + lh + 8; /* room for the high above the bar */
    r.bar_bot = r.precip - lh - 10;         /* and the low below it */
    return r;
}

static void deg(char *out, size_t n, float t)
{
    snprintf(out, n, "%d\xC2\xB0", (int)lroundf(t));
}

static void week_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    time_t fetched;
    const yr_forecast_t *fc = s_loc >= 0 ? weather_forecast(s_loc, &fetched) : NULL;
    if (fc == NULL || fc->day_count == 0) {
        return;
    }
    const int lh = lv_font_get_line_height(g_font_body);
    const int n = fc->day_count < WEEK_DAYS ? fc->day_count : WEEK_DAYS;
    const int w = card_w();
    const week_rows_t r = rows();
    const lv_color_t c_txt = lv_color_hex(dark() ? 0xDDE6EE : 0x1B2631);
    const lv_color_t c_dim = lv_color_hex(dark() ? 0x8AA0B4 : 0x5D6D7E);
    const lv_color_t c_card = lv_color_hex(dark() ? 0x1E2A36 : 0xECEFF2);
    const lv_color_t c_today = lv_color_hex(dark() ? 0x2A3A4A : 0xDCE6F0);
    const lv_color_t c_warm = lv_palette_main(LV_PALETTE_ORANGE);
    const lv_color_t c_cold = dark() ? lv_palette_lighten(LV_PALETTE_BLUE, 2) : lv_palette_darken(LV_PALETTE_BLUE, 2);
    const lv_color_t c_rain = lv_palette_main(LV_PALETTE_BLUE);

    /* One temperature scale for the week. */
    float lo = 1000, hi = -1000;
    for (int d = 0; d < n; d++) {
        lo = fminf(lo, fc->days[d].temp_min_c);
        hi = fmaxf(hi, fc->days[d].temp_max_c);
    }
    if (hi - lo < 4) {
        hi = lo + 4;
    }
    const time_t now = time(NULL);
    struct tm today_lt;
    localtime_r(&now, &today_lt);

    static const char *const WDAY[] = { "s\xC3\xB8ndag", "mandag", "tirsdag", "onsdag", "torsdag", "fredag",
                                        "l\xC3\xB8rdag" };
    for (int d = 0; d < n; d++) {
        const yr_day_t *dy = &fc->days[d];
        const int x = WEEK_X + d * (w + WEEK_GAP);
        const time_t st = (time_t)dy->start;
        struct tm lt;
        localtime_r(&st, &lt);
        const bool is_today = lt.tm_yday == today_lt.tm_yday && lt.tm_year == today_lt.tm_year;
        draw_rect(layer, x, WEEK_TOP, x + w - 1, BOARD_LCD_V_RES - 6, 8, is_today ? c_today : c_card);

        char buf[24];
        draw_text(layer, is_today ? "I dag" : WDAY[lt.tm_wday], x, r.day, w, LV_TEXT_ALIGN_CENTER, c_txt);
        snprintf(buf, sizeof(buf), "%d.%d.", lt.tm_mday, lt.tm_mon + 1);
        draw_text(layer, buf, x, r.date, w, LV_TEXT_ALIGN_CENTER, c_dim);

        /* The range: the high above the bar, the low below it. */
        const int span = r.bar_bot - r.bar_top;
        const int y1 = r.bar_top + (int)((hi - dy->temp_max_c) / (hi - lo) * span);
        int y2 = r.bar_top + (int)((hi - dy->temp_min_c) / (hi - lo) * span);
        if (y2 - y1 < 6) {
            y2 = y1 + 6;
        }
        const int cx = x + w / 2;
        draw_rect(layer, cx - WEEK_BAR_W / 2, y1, cx + WEEK_BAR_W / 2 - 1, y2, WEEK_BAR_W / 2,
                  dy->temp_max_c > 0 ? c_warm : c_cold);
        deg(buf, sizeof(buf), dy->temp_max_c);
        draw_text(layer, buf, x, y1 - lh - 2, w, LV_TEXT_ALIGN_CENTER, dy->temp_max_c > 0 ? c_warm : c_cold);
        deg(buf, sizeof(buf), dy->temp_min_c);
        draw_text(layer, buf, x, y2 + 2, w, LV_TEXT_ALIGN_CENTER, c_cold);

        if (dy->precip_mm >= 0.1f) {
            snprintf(buf, sizeof(buf), "%.1f mm", (double)dy->precip_mm);
            char *dot = strchr(buf, '.');
            if (dot) {
                *dot = ',';
            }
            draw_text(layer, buf, x, r.precip, w, LV_TEXT_ALIGN_CENTER, c_rain);
        } else {
            draw_text(layer, "0 mm", x, r.precip, w, LV_TEXT_ALIGN_CENTER, c_dim);
        }
        snprintf(buf, sizeof(buf), "%d m/s", (int)lroundf(dy->wind_max_ms));
        draw_text(layer, buf, x, r.wind, w, LV_TEXT_ALIGN_CENTER, c_dim);
    }
}

/* Icons and labels for s_loc's forecast (adapter lock held). */
static void week_show(void)
{
    time_t fetched;
    const yr_forecast_t *fc = weather_forecast(s_loc, &fetched);
    const int n = fc ? (fc->day_count < WEEK_DAYS ? fc->day_count : WEEK_DAYS) : 0;
    const int w = card_w();
    for (int d = 0; d < WEEK_DAYS; d++) {
        if (d < n && fc->days[d].symbol_code[0]) {
            char path[64];
            snprintf(path, sizeof(path), "F:%s.png", fc->days[d].symbol_code);
            lv_image_set_src(s_icon[d], path);
            lv_obj_set_pos(s_icon[d], WEEK_X + d * (w + WEEK_GAP) + (w - WEEK_ICON) / 2, rows().icon);
            lv_obj_clear_flag(s_icon[d], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_icon[d], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (fc != NULL && fetched > 0) {
        struct tm lt;
        localtime_r(&fetched, &lt);
        lv_label_set_text_fmt(s_updated, "Oppdatert kl. %02d:%02d", lt.tm_hour, lt.tm_min);
    } else {
        lv_label_set_text(s_updated, "");
    }
    lv_label_set_text(g_status_label, n > 0 ? "" : "Henter v\xC3\xA6rvarsel...");
    lv_obj_invalidate(s_canvas);
}

lv_obj_t *week_build(lv_obj_t *screen)
{
    s_root = screen_root_create(screen);
    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, week_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, WEEK_X, 4);
    lv_obj_set_width(s_title, 520);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    s_updated = lv_label_create(s_root);
    lv_obj_align(s_updated, LV_ALIGN_TOP_RIGHT, -WEEK_X, 4);
    lv_label_set_text(s_updated, "");

    for (int d = 0; d < WEEK_DAYS; d++) {
        s_icon[d] = lv_image_create(s_root);
        lv_obj_set_size(s_icon[d], WEEK_ICON, WEEK_ICON);
        lv_obj_add_flag(s_icon[d], LV_OBJ_FLAG_HIDDEN);
    }
    return s_root;
}

void week_enter(int loc)
{
    s_loc = loc;
    lv_label_set_text_fmt(s_title, "7 dager i %s", g_cfg->locations[loc].name);
    week_show();
}

uint32_t week_poll(int loc, int for_view)
{
    weather_refresh(loc, for_view);
    time_t fetched;
    bool shown = false;
    if (lock_for_view(for_view)) {
        week_show();
        shown = weather_forecast(loc, &fetched) != NULL;
        esp_lv_adapter_unlock();
    }
    return shown ? WEEK_POLL_MS : WEEK_RETRY_MS;
}
