/* The clock screen: the time across the middle of the screen (hours and
 * minutes in large digits, the seconds smaller beside them), the date and
 * week number under it, and the temperature indoors from the chip's own
 * sensor (components/chip_temp.c) plus the offset measured against a
 * thermometer on the setup page.
 *
 * One screen for the display. Nothing to fetch: a timer redraws whatever
 * changed, several times a second so no second is skipped. */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "chip_temp.h"
#include "clockscreen.h"
#include "holidays.h"

#define CLK_TIME_PX 180 /* hours and minutes */
#define CLK_SEC_PX  72  /* seconds, and the temperature */
#define CLK_DATE_PX 40
#define CLK_POLL_MS (10 * 60 * 1000)
#define CLK_TICK_MS 200
#define CLK_HHMM    5   /* "HH:MM" */

static lv_obj_t *s_root, *s_time, *s_hhmm[CLK_HHMM], *s_sec, *s_date, *s_temp;
static time_t s_shown = -1; /* the second on show */
static int s_shown_dc = -10000; /* the temperature on show, in tenths; -10000 = redraw, -10001 = none */

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
        if (s_shown_dc != -10001) {
            s_shown_dc = -10001;
            lv_label_set_text(s_temp, "Inne  \xE2\x80\x93");
        }
        return;
    }
    const int dc = (int)lroundf(raw * 10.0f) + g_cfg->temp_offset_dc;
    if (dc != s_shown_dc) {
        s_shown_dc = dc;
        lv_label_set_text_fmt(s_temp, "Inne  %s%d.%d\xC2\xB0", dc < 0 ? "-" : "", (dc < 0 ? -dc : dc) / 10,
                              (dc < 0 ? -dc : dc) % 10);
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
    s_temp = label(s_root, sec_font, fg);
    show_time(time(NULL));
    show_temp();

    lv_timer_create(tick_cb, CLK_TICK_MS, NULL);
    return s_root;
}

void clockscreen_enter(void)
{
    s_shown = -1;
    s_shown_dc = -10000;
    show_time(time(NULL));
    show_temp();
}

uint32_t clockscreen_poll(int for_view)
{
    (void)for_view;
    return CLK_POLL_MS;
}
