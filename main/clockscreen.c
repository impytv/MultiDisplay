/* The clock screen: the time across the middle of the screen (hours and
 * minutes in large digits, the seconds smaller beside them), the date and
 * week number under it, and two temperatures: indoors (unless unticked,
 * g_cfg->clk_in) from the chip's own sensor (components/chip_temp.c) plus the offset measured against a
 * thermometer on the setup page, and outdoors at the location chosen there
 * (g_cfg->clk_out; none, and the line shows indoors only) from MET's
 * nowcast (its temperature now, which MET corrects with the observations
 * around, Netatmo stations among them).
 *
 * One screen for the display. A timer redraws whatever changed, several
 * times a second so no second is skipped; the nowcast is fetched every 10
 * minutes while the screen is on show. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app.h"
#include "chip_temp.h"
#include "clockscreen.h"
#include "diag.h"
#include "holidays.h"
#include "yr_client.h"

static const char *TAG = "clock";

#define CLK_TIME_PX 180 /* hours and minutes */
#define CLK_SEC_PX  72  /* seconds, and the temperature */
#define CLK_DATE_PX 40
#define CLK_POLL_MS (10 * 60 * 1000) /* the outdoor temperature */
#define CLK_RETRY_MS 30000
#define CLK_OUT_KEEP_MS (30 * 60 * 1000) /* older than this isn't shown */
#define CLK_TICK_MS 200
#define CLK_HHMM    5   /* "HH:MM" */
#define CLK_NONE    -10001 /* no temperature to show */
#define CLK_REDRAW  -10000

static lv_obj_t *s_root, *s_time, *s_hhmm[CLK_HHMM], *s_sec, *s_date, *s_temp, *s_out;
static time_t s_shown = -1; /* the second on show */
static int s_shown_dc = CLK_REDRAW; /* the temperatures on show, whole degrees, or CLK_NONE / CLK_REDRAW */
static int s_shown_out = CLK_REDRAW;

/* The outdoor temperature: set by the weather task under the adapter lock. */
static yr_nowcast_t *s_nowcast;
static http_cache_t s_nc_http;
static float s_out_c;
static fetch_stamp_t s_out_at;
static backoff_t s_out_fail;

static bool dark(void)
{
    return g_cfg->theme == APP_THEME_DARK;
}

/* A label in `font` and `colour`, sized to its text. */
static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    return l;
}

static void show_time(time_t now)
{
    static const char *const WD[] = { "s\xC3\xB8ndag", "mandag", "tirsdag", "onsdag", "torsdag", "fredag",
                                      "l\xC3\xB8rdag" };
    static const char *const MON[] = { "januar", "februar", "mars", "april", "mai", "juni", "juli", "august",
                                       "september", "oktober", "november", "desember" };
    char buf[64];
    if (now <= PLAUSIBLE_EPOCH_S) {
        for (int i = 0; i < CLK_HHMM; i++) {
            lv_label_set_text(s_hhmm[i], i == 2 ? ":" : "-");
        }
        lv_label_set_text(s_sec, ":--");
        lv_label_set_text(s_date, "Klokken er ikke stilt enn\xC3\xA5");
        return;
    }
    struct tm lt;
    localtime_r(&now, &lt);
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    for (int i = 0; i < CLK_HHMM; i++) {
        const char ch[2] = { buf[i], '\0' };
        if (strcmp(lv_label_get_text(s_hhmm[i]), ch) != 0) {
            lv_label_set_text(s_hhmm[i], ch);
        }
    }
    lv_label_set_text_fmt(s_sec, ":%02d", lt.tm_sec);
    snprintf(buf, sizeof(buf), "%s %d. %s %d  \xC2\xB7  uke %d", WD[lt.tm_wday], lt.tm_mday, MON[lt.tm_mon],
             lt.tm_year + 1900, iso_week(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday));
    buf[0] = (char)(buf[0] - 'a' + 'A'); /* weekdays start with a-z (not ø) */
    if (strcmp(lv_label_get_text(s_date), buf) != 0) {
        lv_label_set_text(s_date, buf);
    }
}

static void show_temp(void)
{
    float raw;
    if (!chip_temp_raw(&raw)) {
        if (s_shown_dc != CLK_NONE) {
            s_shown_dc = CLK_NONE;
            lv_label_set_text(s_temp, "Inne  \xE2\x80\x93");
        }
        return;
    }
    /* Whole degrees; the setup page shows the tenths for calibrating. */
    const int dc = (int)lroundf(raw * 10.0f) + g_cfg->temp_offset_dc;
    const int deg = (int)lroundf(dc / 10.0f);
    if (deg != s_shown_dc) {
        s_shown_dc = deg;
        lv_label_set_text_fmt(s_temp, "Inne  %d\xC2\xB0", deg);
    }
}

static void show_out(void)
{
    const int deg = stamp_fresh(&s_out_at, CLK_OUT_KEEP_MS) ? (int)lroundf(s_out_c) : CLK_NONE;
    if (deg == s_shown_out) {
        return;
    }
    s_shown_out = deg;
    if (deg == CLK_NONE) {
        lv_label_set_text(s_out, "Ute  \xE2\x80\x93");
    } else {
        lv_label_set_text_fmt(s_out, "Ute  %d\xC2\xB0", deg);
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    const time_t now = time(NULL);
    if (now != s_shown) {
        s_shown = now;
        show_time(now);
        show_temp();
        show_out();
    }
}

lv_obj_t *clockscreen_build(lv_obj_t *screen)
{
    const lv_font_t *time_font = load_font(CLK_TIME_PX, true);
    const lv_font_t *sec_font = load_font(CLK_SEC_PX, true);
    const lv_font_t *date_font = load_font(CLK_DATE_PX, false);
    const uint32_t fg = dark() ? 0xEEF2F5 : 0x1B2631;
    const uint32_t muted = dark() ? 0x9AA7B2 : 0x5A6875;

    s_root = screen_root_create(screen);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_root, 0, 0);

    /* HH:MM in cells as wide as the widest digit, so the time doesn't shift
     * as it changes, and the seconds beside it on the same baseline. */
    s_time = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_time);
    lv_obj_clear_flag(s_time, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    int digit_w = 0;
    for (uint32_t ch = '0'; ch <= '9'; ch++) {
        const int w = lv_font_get_glyph_width(time_font, ch, 0);
        digit_w = w > digit_w ? w : digit_w;
    }
    const int colon_w = lv_font_get_glyph_width(time_font, ':', 0) + 8;
    const int time_h = lv_font_get_line_height(time_font);
    int x = 0;
    for (int i = 0; i < CLK_HHMM; i++) {
        const int w = i == 2 ? colon_w : digit_w;
        s_hhmm[i] = label(s_time, time_font, fg);
        lv_obj_set_style_text_align(s_hhmm[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_hhmm[i], x, 0);
        lv_obj_set_width(s_hhmm[i], w);
        lv_label_set_text(s_hhmm[i], i == 2 ? ":" : "-");
        x += w;
    }
    s_sec = label(s_time, sec_font, muted);
    lv_label_set_text(s_sec, ":88");
    lv_obj_update_layout(s_sec);
    const int sec_w = lv_obj_get_width(s_sec) + 8;
    /* Baselines level: each font's ascent above it. */
    lv_obj_set_pos(s_sec, x + 6, time_font->line_height - time_font->base_line - (sec_font->line_height -
                                                                                   sec_font->base_line));
    lv_obj_set_size(s_time, x + 6 + sec_w, time_h);

    s_date = label(s_root, date_font, muted);
    lv_obj_set_style_margin_bottom(s_date, 24, 0);

    /* Inside and outside side by side. */
    lv_obj_t *temps = lv_obj_create(s_root);
    lv_obj_remove_style_all(temps);
    lv_obj_clear_flag(temps, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(temps, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(temps, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(temps, 64, 0);
    s_temp = label(temps, sec_font, fg);
    s_out = label(temps, sec_font, fg);
    /* The settings are read once: saving restarts. */
    if (!g_cfg->clk_in) {
        lv_obj_add_flag(s_temp, LV_OBJ_FLAG_HIDDEN);
    }
    if (g_cfg->clk_out == 0) {
        lv_obj_add_flag(s_out, LV_OBJ_FLAG_HIDDEN);
    }

    s_nowcast = heap_caps_calloc(1, sizeof(*s_nowcast), MALLOC_CAP_SPIRAM);
    assert(s_nowcast != NULL);
    show_time(time(NULL));
    show_temp();
    show_out();

    lv_timer_create(tick_cb, CLK_TICK_MS, NULL);
    return s_root;
}

void clockscreen_enter(void)
{
    s_shown = -1;
    s_shown_dc = CLK_REDRAW;
    s_shown_out = CLK_REDRAW;
    show_time(time(NULL));
    show_temp();
    show_out();
}

uint32_t clockscreen_poll(int for_view)
{
    if (g_cfg->clk_out == 0 || g_cfg->clk_out > g_cfg->location_count) {
        return CLK_POLL_MS; /* no outdoor temperature asked for */
    }
    /* Woken early (a tap, the screen coming back): nothing due yet. */
    if (backoff_waiting(&s_out_fail, CLK_RETRY_MS)) {
        return CLK_RETRY_MS;
    }
    if (stamp_fresh(&s_out_at, CLK_POLL_MS)) {
        return CLK_POLL_MS;
    }
    const app_location_t *loc = &g_cfg->locations[g_cfg->clk_out - 1];
    if (!s_out_at.valid) {
        memset(&s_nc_http, 0, sizeof(s_nc_http)); /* nothing held to compare with */
    }
    esp_err_t err = yr_client_fetch_nowcast(atof(loc->lat), atof(loc->lon), s_nowcast, &s_nc_http, false);
    /* One request every 10 minutes: not worth a kept-alive TLS connection. */
    http_met_close();
    float c = 0;
    bool got = false;
    if (err == ESP_OK) {
        for (int i = 0; i < s_nowcast->point_count && !got; i++) {
            if (s_nowcast->points[i].has_instant_details) {
                c = s_nowcast->points[i].air_temperature_c;
                got = true;
            }
        }
    }
    if (got || err == HTTP_NOT_MODIFIED) {
        diag_ok(DIAG_NOWCAST);
        backoff_ok(&s_out_fail);
    } else {
        diag_fail(DIAG_NOWCAST, err == ESP_OK ? ESP_ERR_NOT_FOUND : err);
        backoff_fail(&s_out_fail);
        memset(&s_nc_http, 0, sizeof(s_nc_http)); /* nothing usable held */
        ESP_LOGW(TAG, "No outdoor temperature for %s: %s", loc->name,
                 err == ESP_OK ? "none in the nowcast" : esp_err_to_name(err));
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (got) {
            s_out_c = c;
        }
        if (got || err == HTTP_NOT_MODIFIED) {
            stamp_now(&s_out_at);
        }
        esp_lv_adapter_unlock();
    }
    if (lock_for_view(for_view)) {
        show_out();
        esp_lv_adapter_unlock();
    }
    return got || err == HTTP_NOT_MODIFIED ? CLK_POLL_MS : backoff_ms(&s_out_fail, CLK_RETRY_MS);
}
