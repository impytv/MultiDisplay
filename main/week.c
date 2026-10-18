/* The week screen: the big clock top right, and seven day cards from the location's forecast (the days
 * yr_client sums up): the weekday, MET's symbol for the day, the day's
 * temperature range as a bar on a scale shared by the whole week, with the
 * high above and the low below it (orange above zero, blue below), the
 * precipitation total with MET's high end ("0,4-2,1 mm") and the strongest
 * wind with the strongest gust ("5 (12) m/s"; MET gives gusts for the
 * first two days or so). Uses the weather screen's forecast cache (weather.c). */

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
#define WEEK_TOP       60 /* below the big clock, as on the weather screen */
#define WEEK_GAP       6
#define WEEK_ICON      64
#define WEEK_BAR_W     14
#define WEEK_POLL_MS   (10 * 60 * 1000)
#define WEEK_RETRY_MS  20000

static lv_obj_t *s_root, *s_canvas, *s_title, *s_updated;
static big_clock_t s_clock;
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
    int day, date, icon, bar_top, bar_bot, precip, wind, bottom;
} week_rows_t;

static week_rows_t rows(void)
{
    const int lh = lv_font_get_line_height(g_font_body);
    week_rows_t r;
    r.day = WEEK_TOP + 6;
    r.date = r.day + lh;
    r.icon = r.date + lh + 4;
    r.bottom = BOARD_LCD_V_RES - lh - 10; /* above the "updated" line at the bottom right */
    r.wind = r.bottom - lh - 6;
    r.precip = r.wind - lh - 2;
    r.bar_top = r.icon + WEEK_ICON + lh + 8; /* room for the high above the bar */
    r.bar_bot = r.precip - lh - 10;         /* and the low below it */
    return r;
}

static void deg(char *out, size_t n, float t)
{
    snprintf(out, n, "%d\xC2\xB0", (int)lroundf(t));
}

/* An amount of precipitation: "2,1", or "12" from 10 mm, or whole with
 * `whole`. */
static void mm(char *out, size_t n, float v, bool whole)
{
    if (whole || v >= 9.95f) {
        snprintf(out, n, "%d", (int)lroundf(v));
    } else {
        snprintf(out, n, "%.1f", (double)v);
        char *dot = strchr(out, '.');
        if (dot) {
            *dot = ',';
        }
    }
}

/* Whether two amounts show differently at a tenth of a millimetre. */
static bool mm_differ(float a, float b)
{
    return lroundf(a * 10) != lroundf(b * 10);
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
        draw_rect(layer, x, WEEK_TOP, x + w - 1, r.bottom, 8, is_today ? c_today : c_card);

        char buf[40];
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
        const int bx1 = cx - WEEK_BAR_W / 2, bx2 = cx + WEEK_BAR_W / 2 - 1, br = WEEK_BAR_W / 2;
        draw_rect(layer, bx1, y1, bx2, y2, br, dy->temp_max_c > 0 ? c_warm : c_cold);
        if (dy->temp_max_c > 0 && dy->temp_min_c < 0) {
            /* Blue from zero down, square at the top. */
            const int y0 = r.bar_top + (int)(hi / (hi - lo) * span);
            if (y0 < y2) {
                draw_rect(layer, bx1, y0, bx2, y2, br, c_cold);
                draw_rect(layer, bx1, y0, bx2, y0 + br < y2 ? y0 + br : y2, 0, c_cold);
            }
        }
        deg(buf, sizeof(buf), dy->temp_max_c);
        draw_text(layer, buf, x, y1 - lh - 2, w, LV_TEXT_ALIGN_CENTER, dy->temp_max_c > 0 ? c_warm : c_cold);
        deg(buf, sizeof(buf), dy->temp_min_c);
        draw_text(layer, buf, x, y2 + 2, w, LV_TEXT_ALIGN_CENTER, dy->temp_min_c > 0 ? c_warm : c_cold);

        /* "1,2 mm", or "0,4-2,1 mm" when MET's high end is more; whole
         * numbers if that is too wide. */
        char a[12], b[12];
        const bool range = mm_differ(dy->precip_mm, dy->precip_max_mm);
        for (int whole = 0; whole < 2; whole++) {
            mm(a, sizeof(a), dy->precip_mm, whole);
            mm(b, sizeof(b), dy->precip_max_mm, whole);
            if (range && strcmp(a, b) != 0) {
                snprintf(buf, sizeof(buf), "%s\xE2\x80\x93%s mm", strcmp(a, "0,0") == 0 ? "0" : a, b);
            } else {
                snprintf(buf, sizeof(buf), "%s mm", strcmp(a, "0,0") == 0 ? "0" : a);
            }
            if (draw_text_w(buf) <= w - 8) {
                break;
            }
        }
        draw_text(layer, buf, x, r.precip, w, LV_TEXT_ALIGN_CENTER,
                  dy->precip_max_mm >= 0.05f ? c_rain : c_dim);
        const int wind = (int)lroundf(dy->wind_max_ms), gust = (int)lroundf(dy->gust_max_ms);
        if (gust > wind) {
            snprintf(buf, sizeof(buf), "%d (%d) m/s", wind, gust);
        } else {
            snprintf(buf, sizeof(buf), "%d m/s", wind);
        }
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

/* Every second while the week is on show. */
static void week_clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        big_clock_set(&s_clock, time(NULL));
    }
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
    big_clock_create(&s_clock, s_root, WEEK_X);
    s_updated = lv_label_create(s_root);
    lv_obj_align(s_updated, LV_ALIGN_BOTTOM_RIGHT, -WEEK_X, -4);
    lv_label_set_text(s_updated, "");

    for (int d = 0; d < WEEK_DAYS; d++) {
        s_icon[d] = lv_image_create(s_root);
        lv_obj_set_size(s_icon[d], WEEK_ICON, WEEK_ICON);
        lv_obj_add_flag(s_icon[d], LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_create(week_clock_timer_cb, 1000, NULL);
    return s_root;
}

void week_enter(int loc)
{
    s_loc = loc;
    lv_label_set_text_fmt(s_title, "7 dager i %s", g_cfg->locations[loc].name);
    big_clock_set(&s_clock, time(NULL));
    week_show();
}

uint32_t week_poll(int loc, int for_view)
{
    /* Its own location's forecast and on screen, then the others'. */
    weather_refresh(loc, false, for_view);
    time_t fetched;
    bool shown = false;
    if (lock_for_view(for_view)) {
        week_show();
        shown = weather_forecast(loc, &fetched) != NULL;
        esp_lv_adapter_unlock();
    }
    if (shown) {
        weather_refresh(loc, true, for_view);
    }
    return shown ? WEEK_POLL_MS : WEEK_RETRY_MS;
}
