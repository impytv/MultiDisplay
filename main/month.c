/* The month calendar: the big clock top right, and five weeks from the
 * Monday of this one as a grid - a row per week with its number on the
 * left, a column per weekday (Monday first). Each day shows its date, the
 * month's short name on the first of a month (and the first day shown),
 * and a holiday's name. Sundays and holidays have their date in red
 * (components/holidays.c, from norskkalender.no); today is outlined, and
 * days already past are faded. Months alternate in shade. If ticked on
 * the setup page, the moon's phases are drawn beside the date, and other
 * events in the sky (components/astro.c: equinoxes, solstices, supermoons,
 * meteor showers, and eclipses seen from the first location) named under
 * it after any holiday.
 *
 * Buttons by the clock step four weeks back or ahead (the last week shown
 * becomes the first), and "I dag" goes back to this week; so does leaving
 * the screen, or MON_BACK_MS without a touch. The tap layer over every
 * screen takes the touches, so main.c hands them here (month_tap) first.
 *
 * One screen for the display. Nothing to fetch: redrawn when the date
 * changes (checked with the clock, every second). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"

#include "app.h"
#include "astro.h"
#include "draw.h"
#include "holidays.h"
#include "month.h"
#include "waveshare_rgb_lcd_port.h"

#define MON_X       12
#define MON_TOP     60 /* below the big clock */
#define MON_GAP     4
#define MON_WEEKS   5
#define MON_DATE_PX 30 /* the date in each day */
#define MON_NOTE_PX 14 /* the month and holiday names, the sky's events */
#define MON_POLL_MS (10 * 60 * 1000)
#define MON_STEP    4                /* weeks a button moves */
#define MON_MAX_OFF (2 * 52)         /* how far either way, in weeks */
#define MON_BACK_MS (5 * 60 * 1000)  /* back to this week after this long untouched */
#define MON_BTN_H   44
#define MON_BTN_W   60
#define MON_MOON_R  8  /* the moon's phase beside the date */
#define MON_ASTRO   4  /* events a day can have */

static lv_obj_t *s_root, *s_canvas, *s_title;
static big_clock_t s_clock;
static const lv_font_t *s_date_font, *s_note_font;
static int s_shown_day = -1; /* year * 1000 + day of the year drawn, 0 before the clock is set */
static int s_offset;         /* weeks from this one, in steps of MON_STEP */
static uint32_t s_touch_ms;  /* the last press on a button */
enum { BTN_TODAY, BTN_BACK, BTN_AHEAD, BTN_COUNT };
static lv_obj_t *s_btn[BTN_COUNT];

/* The sky's events for each day shown, worked out when the days change
 * (3.5 KB, in PSRAM). */
static EXT_RAM_BSS_ATTR struct {
    astro_item_t it[MON_ASTRO];
    int n;
} s_astro[MON_WEEKS * 7];

static const char *const MONTH[] = { "januar", "februar", "mars", "april", "mai", "juni", "juli",
                                     "august", "september", "oktober", "november", "desember" };
static const char *const MONTH_SHORT[] = { "jan.", "feb.", "mars", "apr.", "mai", "juni", "juli",
                                           "aug.", "sep.", "okt.", "nov.", "des." };
static const char *const WDAY[] = { "Man", "Tir", "Ons", "Tor", "Fre", "L\xC3\xB8r", "S\xC3\xB8n" };

/* Holiday and meteor shower names too long for a day at MON_NOTE_PX, as
 * shown: shortened, or with a hyphen where they may break. */
static const char *const SHORTER[][2] = {
    { "Kristi Himmelfartsdag", "Kristi himmelfart" },
    { "Grunnlovsdag", "Grunnlovs-dag" },
    { "Skj\xC3\xA6rtorsdag", "Skj\xC3\xA6r-torsdag" },
    { "Palmes\xC3\xB8ndag", "Palme-s\xC3\xB8ndag" },
    { "Kvadrantidene", "Kvadrant-idene" },
};

static const char *shown_name(const char *name)
{
    for (size_t i = 0; i < sizeof(SHORTER) / sizeof(SHORTER[0]); i++) {
        if (strcmp(name, SHORTER[i][0]) == 0) {
            return SHORTER[i][1];
        }
    }
    return name;
}

static bool dark(void)
{
    return g_cfg->theme == APP_THEME_DARK;
}

/* Today's key for s_shown_day, 0 before the clock is set. */
static int day_key(time_t now, struct tm *lt)
{
    if (now <= PLAUSIBLE_EPOCH_S) {
        return 0;
    }
    localtime_r(&now, lt);
    return (lt->tm_year + 1900) * 1000 + lt->tm_yday;
}

static int tm_key(const struct tm *t)
{
    return (t->tm_year + 1900) * 1000 + t->tm_yday;
}

/* `txt` in `font` in the box x, y, w, h: wrapped, and cut at the box. */
static void text(lv_layer_t *layer, const lv_font_t *font, const char *txt, int x, int y, int w, int h,
                 lv_text_align_t align, lv_color_t color)
{
    lv_area_t a = { x, y, x + w - 1, y + h - 1 };
    const lv_area_t clip = layer->_clip_area;
    const lv_area_t cut = { LV_MAX(clip.x1, a.x1), LV_MAX(clip.y1, a.y1), LV_MIN(clip.x2, a.x2),
                            LV_MIN(clip.y2, a.y2) };
    if (cut.x1 > cut.x2 || cut.y1 > cut.y2) {
        return;
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font;
    d.color = color;
    d.text = txt;
    d.text_local = 1;
    d.align = align;
    layer->_clip_area = cut; /* a draw task takes the clip area when it is created */
    lv_draw_label(layer, &d, &a);
    layer->_clip_area = clip;
}

/* The grid's columns, from the body font. */
typedef struct {
    int wk_w;   /* the week number column */
    int day_x;  /* the first day's left edge */
    int day_w;
    int grid_y; /* the first week's top */
    int row_h;  /* a week, gap included */
} month_grid_t;

static month_grid_t grid(void)
{
    month_grid_t g;
    const int lh = lv_font_get_line_height(g_font_body);
    const int w88 = draw_text_w("88"), wuke = draw_text_w("Uke");
    g.wk_w = (w88 > wuke ? w88 : wuke) + 8;
    g.day_x = MON_X + g.wk_w + MON_GAP;
    g.day_w = (BOARD_LCD_H_RES - MON_X - g.day_x - 6 * MON_GAP) / 7;
    g.grid_y = MON_TOP + lh + 4;
    g.row_h = (BOARD_LCD_V_RES - 6 - g.grid_y + MON_GAP) / MON_WEEKS;
    return g;
}

/* The first day shown: the Monday of the week of `today` (midday, to keep
 * clear of daylight saving changes when stepping a day at a time). */
static struct tm first_day(const struct tm *today)
{
    struct tm t = *today;
    t.tm_hour = 12;
    t.tm_min = t.tm_sec = 0;
    t.tm_mday -= (t.tm_wday + 6) % 7;
    t.tm_isdst = -1;
    mktime(&t);
    return t;
}

static struct tm add_days(const struct tm *t0, int days)
{
    struct tm t = *t0;
    t.tm_mday += days;
    t.tm_isdst = -1;
    mktime(&t);
    return t;
}

/* Which events come first under a date when there is room for few. */
static int rank(astro_kind_t k)
{
    return k == ASTRO_SOLAR_ECLIPSE || k == ASTRO_LUNAR_ECLIPSE ? 0
         : k >= ASTRO_MARCH_EQUINOX && k <= ASTRO_DECEMBER_SOLSTICE ? 1
         : k == ASTRO_SUPERMOON ? 2
         : k == ASTRO_METEORS ? 3 : 4;
}

/* The text for an event under the date, or NULL for none (the phases are
 * drawn, not named). Hyphens where the names may break. */
static const char *astro_text(const astro_item_t *it, char *buf, size_t n)
{
    switch (it->kind) {
    case ASTRO_MARCH_EQUINOX:
        return "V\xC3\xA5r-jevnd\xC3\xB8gn";
    case ASTRO_JUNE_SOLSTICE:
        return "Sommer-solverv";
    case ASTRO_SEPTEMBER_EQUINOX:
        return "H\xC3\xB8st-jevnd\xC3\xB8gn";
    case ASTRO_DECEMBER_SOLSTICE:
        return "Vinter-solverv";
    case ASTRO_SUPERMOON:
        return "Superm\xC3\xA5ne";
    case ASTRO_METEORS:
        return shown_name(astro_shower_name(it->value));
    case ASTRO_LUNAR_ECLIPSE:
        return it->value == 2 ? "Total m\xC3\xA5ne-form\xC3\xB8rkelse" : "M\xC3\xA5ne-form\xC3\xB8rkelse";
    case ASTRO_SOLAR_ECLIPSE:
        if (it->value >= 100) {
            return "Total sol-form\xC3\xB8rkelse";
        }
        snprintf(buf, n, it->annular ? "Ringformet sol-form\xC3\xB8rkelse" : "%d %% sol-form\xC3\xB8rkelse",
                 it->value);
        return buf;
    default:
        return NULL;
    }
}

/* The moon's phase as a disc r around cx, cy: dark, with the lit part
 * (as seen from the north: waxing on the right) in light. */
static void moon(lv_layer_t *layer, int cx, int cy, astro_kind_t phase, lv_color_t dark_c, lv_color_t lit)
{
    if (phase == ASTRO_FULL_MOON) {
        draw_dot(layer, cx, cy, MON_MOON_R, lit);
        return;
    }
    draw_dot(layer, cx, cy, MON_MOON_R, dark_c);
    if (phase == ASTRO_NEW_MOON) {
        return;
    }
    /* Half of it: the lit disc, clipped to its side. */
    const lv_area_t clip = layer->_clip_area;
    const lv_area_t half = phase == ASTRO_FIRST_QUARTER
                         ? (lv_area_t){ cx, cy - MON_MOON_R - 1, cx + MON_MOON_R + 1, cy + MON_MOON_R + 1 }
                         : (lv_area_t){ cx - MON_MOON_R - 1, cy - MON_MOON_R - 1, cx - 1, cy + MON_MOON_R + 1 };
    const lv_area_t cut = { LV_MAX(clip.x1, half.x1), LV_MAX(clip.y1, half.y1), LV_MIN(clip.x2, half.x2),
                            LV_MIN(clip.y2, half.y2) };
    if (cut.x1 <= cut.x2 && cut.y1 <= cut.y2) {
        layer->_clip_area = cut;
        draw_dot(layer, cx, cy, MON_MOON_R, lit);
        layer->_clip_area = clip;
    }
}

/* Height of `txt` in `font` wrapped to `w`. */
static int text_h(const lv_font_t *font, const char *txt, int w)
{
    lv_point_t sz;
    lv_text_get_size(&sz, txt, font, 0, 0, w, LV_TEXT_FLAG_NONE);
    return sz.y;
}

static void month_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    struct tm today;
    if (day_key(time(NULL), &today) == 0) {
        return;
    }
    const month_grid_t g = grid();
    const int lh = lv_font_get_line_height(g_font_body);
    const int date_lh = lv_font_get_line_height(s_date_font);
    const int note_lh = lv_font_get_line_height(s_note_font);
    const lv_color_t c_txt = lv_color_hex(dark() ? 0xDDE6EE : 0x1B2631);
    const lv_color_t c_dim = lv_color_hex(dark() ? 0x8AA0B4 : 0x5D6D7E);
    const lv_color_t c_red = lv_color_hex(dark() ? 0xFF6B5E : 0xC62828);
    const lv_color_t c_card[2] = { lv_color_hex(dark() ? 0x1E2A36 : 0xECEFF2),
                                   lv_color_hex(dark() ? 0x2A3846 : 0xDDE3EA) };
    const lv_color_t c_today = lv_color_hex(dark() ? 0x2A3A4A : 0xDCE6F0);
    const lv_color_t c_ring = lv_palette_main(LV_PALETTE_BLUE);
    const lv_color_t c_sky = lv_color_hex(dark() ? 0x7FB3E6 : 0x1F5F9E);
    const lv_color_t c_moon_dark = lv_color_hex(dark() ? 0x55606B : 0x8A96A3);
    const lv_color_t c_moon_lit = lv_color_hex(dark() ? 0xF4D35E : 0xE0A800);

    /* The weekdays over the columns, Sunday in red. */
    text(layer, g_font_body, "Uke", MON_X, MON_TOP, g.wk_w, lh, LV_TEXT_ALIGN_CENTER, c_dim);
    for (int d = 0; d < 7; d++) {
        text(layer, g_font_body, WDAY[d], g.day_x + d * (g.day_w + MON_GAP), MON_TOP, g.day_w, lh,
             LV_TEXT_ALIGN_CENTER, d == 6 ? c_red : c_dim);
    }

    const struct tm this_week = first_day(&today);
    const struct tm first = add_days(&this_week, s_offset * 7);
    const int today_key = tm_key(&today);
    const int cell_h = g.row_h - MON_GAP;
    for (int w = 0; w < MON_WEEKS; w++) {
        const int y = g.grid_y + w * g.row_h;
        if (!draw_visible(layer, 0, y, BOARD_LCD_H_RES - 1, y + cell_h - 1)) {
            continue;
        }
        const struct tm mon = add_days(&first, w * 7);
        char buf[48];
        snprintf(buf, sizeof(buf), "%d", iso_week(mon.tm_year + 1900, mon.tm_mon + 1, mon.tm_mday));
        const bool this_wk = s_offset + w == 0;
        text(layer, g_font_body, buf, MON_X, y + (cell_h - lh) / 2, g.wk_w, lh, LV_TEXT_ALIGN_CENTER,
             this_wk ? c_txt : c_dim);

        for (int d = 0; d < 7; d++) {
            const struct tm t = d == 0 ? mon : add_days(&mon, d);
            const int x = g.day_x + d * (g.day_w + MON_GAP);
            const bool is_today = tm_key(&t) == today_key;
            const bool past = tm_key(&t) < today_key;
            const char *holiday = holiday_name(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
            const lv_color_t card = is_today ? c_today : c_card[t.tm_mon % 2];
            if (is_today) {
                draw_rect(layer, x, y, x + g.day_w - 1, y + cell_h - 1, 8, c_ring);
                draw_rect(layer, x + 3, y + 3, x + g.day_w - 4, y + cell_h - 4, 6, card);
            } else {
                draw_rect(layer, x, y, x + g.day_w - 1, y + cell_h - 1, 8, card);
            }

            lv_color_t c_date = (d == 6 || holiday != NULL) ? c_red : c_txt;
            lv_color_t c_note = c_dim, c_hol = c_red;
            if (past) {
                c_date = lv_color_mix(c_date, card, LV_OPA_50);
                c_note = lv_color_mix(c_note, card, LV_OPA_50);
                c_hol = lv_color_mix(c_hol, card, LV_OPA_50);
            }
            const int pad = 8;
            snprintf(buf, sizeof(buf), "%d", t.tm_mday);
            text(layer, s_date_font, buf, x + pad, y + 2, g.day_w - 2 * pad, date_lh, LV_TEXT_ALIGN_LEFT, c_date);
            if (t.tm_mday == 1 || (w == 0 && d == 0)) {
                /* Level with the foot of the date. */
                text(layer, s_note_font, MONTH_SHORT[t.tm_mon], x + pad, y + 2 + date_lh - note_lh - 4,
                     g.day_w - 2 * pad, note_lh, LV_TEXT_ALIGN_RIGHT, c_note);
            }
            /* Under the date (its line has room below the digits): the
             * holiday, then the sky's events, as far as there is room. */
            const int nx = x + 4, nw = g.day_w - 8, bottom = y + cell_h - 1;
            int ny = y + date_lh - 4;
            if (holiday != NULL) {
                const char *name = shown_name(holiday);
                text(layer, s_note_font, name, nx, ny, nw, bottom - ny, LV_TEXT_ALIGN_LEFT, c_hol);
                ny += text_h(s_note_font, name, nw);
            }
            const int idx = w * 7 + d;
            lv_color_t c_ev = past ? lv_color_mix(c_sky, card, LV_OPA_50) : c_sky;
            for (int k = 0; k < s_astro[idx].n; k++) {
                const astro_item_t *it = &s_astro[idx].it[k];
                if (it->kind <= ASTRO_LAST_QUARTER) {
                    if (g_cfg->mon_moon) {
                        lv_point_t sz;
                        lv_text_get_size(&sz, buf, s_date_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                        moon(layer, x + pad + sz.x + 6 + MON_MOON_R, y + 2 + date_lh / 2, it->kind,
                             past ? lv_color_mix(c_moon_dark, card, LV_OPA_50) : c_moon_dark,
                             past ? lv_color_mix(c_moon_lit, card, LV_OPA_50) : c_moon_lit);
                    }
                    continue;
                }
                if (!g_cfg->mon_astro || ny >= bottom - note_lh / 2) {
                    continue;
                }
                char tb[48];
                const char *t = astro_text(it, tb, sizeof(tb));
                if (t != NULL) {
                    text(layer, s_note_font, t, nx, ny, nw, bottom - ny, LV_TEXT_ALIGN_LEFT, c_ev);
                    ny += text_h(s_note_font, t, nw);
                }
            }
        }
    }
}

/* The title and grid for today (adapter lock held). */
static void month_show(void)
{
    struct tm today;
    s_shown_day = day_key(time(NULL), &today);
    if (s_shown_day == 0) {
        lv_label_set_text(s_title, "M\xC3\xA5ned");
        lv_label_set_text(g_status_label, "Venter p\xC3\xA5 klokken...");
    } else {
        /* The months shown: "Oktober - november 2026", "Desember 2026 - januar 2027";
         * with the short names if that is too wide beside the buttons. */
        const struct tm this_week = first_day(&today);
        const struct tm a = add_days(&this_week, s_offset * 7);
        const struct tm b = add_days(&a, MON_WEEKS * 7 - 1);
        char title[64];
        for (int pass = 0; pass < 2; pass++) {
            const char *const *names = pass == 0 ? MONTH : MONTH_SHORT;
            char from[16];
            snprintf(from, sizeof(from), "%s", names[a.tm_mon]);
            from[0] = (char)(from[0] - 'a' + 'A');
            if (a.tm_year != b.tm_year) {
                snprintf(title, sizeof(title), "%s %d \xE2\x80\x93 %s %d", from, a.tm_year + 1900, names[b.tm_mon],
                         b.tm_year + 1900);
            } else {
                snprintf(title, sizeof(title), "%s \xE2\x80\x93 %s %d", from, names[b.tm_mon], b.tm_year + 1900);
            }
            lv_point_t sz;
            lv_text_get_size(&sz, title, g_font_large, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x <= lv_obj_get_width(s_title)) {
                break;
            }
        }
        lv_label_set_text(s_title, title);
        lv_label_set_text(g_status_label, "");

        /* The sky, seen from the first location. */
        const double lat = atof(g_cfg->locations[0].lat), lon = atof(g_cfg->locations[0].lon);
        for (int i = 0; i < MON_WEEKS * 7; i++) {
            s_astro[i].n = 0;
            if (!g_cfg->mon_moon && !g_cfg->mon_astro) {
                continue;
            }
            struct tm d0 = add_days(&a, i), d1 = add_days(&a, i + 1);
            d0.tm_hour = d1.tm_hour = 0;
            d0.tm_isdst = d1.tm_isdst = -1;
            astro_item_t *it = s_astro[i].it;
            const int n = s_astro[i].n = astro_day(mktime(&d0), mktime(&d1), lat, lon, it, MON_ASTRO);
            for (int j = 1; j < n; j++) { /* by rank, keeping time order within one */
                const astro_item_t x = it[j];
                int k = j;
                for (; k > 0 && rank(it[k - 1].kind) > rank(x.kind); k--) {
                    it[k] = it[k - 1];
                }
                it[k] = x;
            }
        }
    }
    for (int i = 0; i < BTN_COUNT; i++) {
        const bool on = s_shown_day != 0 && (i != BTN_TODAY || s_offset != 0);
        if (on) {
            lv_obj_clear_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_invalidate(s_canvas);
}

bool month_tap(int x, int y)
{
    static const int STEP[BTN_COUNT] = { [BTN_BACK] = -MON_STEP, [BTN_AHEAD] = MON_STEP };
    for (int i = 0; i < BTN_COUNT; i++) {
        lv_area_t a;
        lv_obj_get_coords(s_btn[i], &a);
        if (lv_obj_has_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN) || x < a.x1 - 6 || x > a.x2 + 6 || y < a.y1 - 6 ||
            y > a.y2 + 6) {
            continue;
        }
        const int off = i == BTN_TODAY ? 0 : s_offset + STEP[i];
        if (off >= -MON_MAX_OFF && off <= MON_MAX_OFF) {
            s_offset = off;
            month_show();
        }
        s_touch_ms = lv_tick_get();
        return true;
    }
    return false;
}

/* Every second while on show: the clock, and the grid when the date
 * changes (or the clock is first set). */
static void month_clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    const time_t now = time(NULL);
    big_clock_set(&s_clock, now);
    struct tm lt;
    if (s_offset != 0 && lv_tick_get() - s_touch_ms >= MON_BACK_MS) {
        s_offset = 0;
        month_show();
    } else if (day_key(now, &lt) != s_shown_day) {
        month_show();
    }
}

lv_obj_t *month_build(lv_obj_t *screen)
{
    s_date_font = load_font(MON_DATE_PX, true);
    s_note_font = load_font(MON_NOTE_PX, false);
    s_root = screen_root_create(screen);
    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, month_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, MON_X, 4);
    lv_obj_set_width(s_title, 520);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    big_clock_create(&s_clock, s_root, MON_X);

    /* The buttons, left of the clock: "I dag", back, ahead. Only drawn -
     * the touches come through month_tap. */
    lv_obj_update_layout(s_clock.obj);
    static const char *const LABEL[BTN_COUNT] = { "I dag", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT };
    int x = lv_obj_get_x(s_clock.obj) - 16;
    for (int i = BTN_COUNT - 1; i >= 0; i--) {
        s_btn[i] = lv_button_create(s_root);
        lv_obj_remove_flag(s_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_shadow_width(s_btn[i], 0, 0);
        lv_obj_set_style_radius(s_btn[i], 8, 0);
        lv_obj_set_style_bg_color(s_btn[i], lv_color_hex(dark() ? 0x2A3A4A : 0xDCE6F0), 0);
        lv_obj_t *l = lv_label_create(s_btn[i]);
        lv_obj_set_style_text_font(l, i == BTN_TODAY ? g_font_body : &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(dark() ? 0xDDE6EE : 0x1B2631), 0);
        lv_label_set_text(l, LABEL[i]);
        lv_obj_center(l);
        const int w = i == BTN_TODAY ? draw_text_w("I dag") + 28 : MON_BTN_W;
        x -= w;
        lv_obj_set_size(s_btn[i], w, MON_BTN_H);
        lv_obj_set_pos(s_btn[i], x, (56 - MON_BTN_H) / 2);
        lv_obj_add_flag(s_btn[i], LV_OBJ_FLAG_HIDDEN);
        x -= 10;
    }
    lv_obj_set_size(s_title, x - MON_X, lv_font_get_line_height(g_font_large));
    lv_timer_create(month_clock_timer_cb, 1000, NULL);
    return s_root;
}

void month_enter(void)
{
    s_offset = 0;
    big_clock_set(&s_clock, time(NULL));
    month_show();
}

uint32_t month_poll(int for_view)
{
    if (lock_for_view(for_view)) {
        month_show();
        esp_lv_adapter_unlock();
    }
    return MON_POLL_MS;
}
