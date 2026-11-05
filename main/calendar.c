/* The calendar screen: the next two weeks from up to three calendars (the
 * iCal addresses on the setup page, components/cal_client.c), as an agenda
 * in two columns - a heading per day ("I dag", "I morgen", "Lørdag 10.
 * oktober") and a row per event: a bar in its calendar's colour, the time
 * (or "Hele dagen") and the title. Days without events are left out, and
 * events that are over are dropped from today. The display's own events
 * (main/events.c) ticked for the calendar come in among them, all-day, in
 * their kind's colour.
 *
 * One screen for the display, after the overview. Fetched every 15
 * minutes; redrawn every few minutes so the past drops off. */

#include <ctype.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app.h"
#include "cal_client.h"
#include "calendar.h"
#include "diag.h"
#include "draw.h"
#include "events.h"
#include "own_events.h"
#include "waveshare_rgb_lcd_port.h"
#include "nb_text.h"

static const char *TAG = "calendar";

#define CAL_X        12
#define CAL_TOP      54
#define CAL_GAP      24
#define CAL_DAYS     14
#define CAL_FETCH_MS (15 * 60 * 1000)
#define CAL_KEEP_MS  (12 * 3600 * 1000) /* older than this isn't shown */
#define CAL_POLL_MS  (5 * 60 * 1000)
#define CAL_IDLE_MS  (10 * 60 * 1000)   /* with no calendar set up */
#define CAL_RETRY_MS 60000
#define CAL_OWN_MAX  40 /* the display's own events in two weeks */

/* As the dots on the setup page. */
static const uint32_t FEED_COLOUR[APP_CONFIG_CAL_FEEDS] = { 0x2E86DE, 0xE67E22, 0x27AE60 };

static lv_obj_t *s_root, *s_canvas, *s_title, *s_updated;
static cal_list_t *s_list, *s_scratch; /* in PSRAM */
static cal_list_t *s_view; /* s_list and the display's own events: what is drawn */
static fetch_stamp_t s_at;
static uint8_t s_feed_failed; /* calendars not read on the last fetch */
static fetch_stamp_t s_feed_at[APP_CONFIG_CAL_FEEDS]; /* when each was last read */
static bool s_retried;        /* the quick retry after a failed calendar was made */
static bool s_failed;         /* the last poll failed while older data is shown */

uint32_t calendar_feed_colour(int feed)
{
    if (OWN_IS_FEED(feed)) {
        return own_kind_colour(feed - OWN_FEED(0));
    }
    return FEED_COLOUR[(unsigned)feed % APP_CONFIG_CAL_FEEDS];
}

static lv_color_t text_colour(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0xDDE6EE : 0x1B2631);
}

static lv_color_t dim_colour(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0x8AA0B4 : 0x5D6D7E);
}

static bool any_calendar(void)
{
    for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
        if (g_cfg->cal_url[i][0] != '\0') {
            return true;
        }
    }
    return false;
}

static const cal_list_t *shown(void)
{
    return stamp_fresh(&s_at, CAL_KEEP_MS) ? s_list : NULL;
}

/* What to draw: the calendars' events and the display's own, or NULL
 * while the calendars set up haven't been read. */
static const cal_list_t *view(void)
{
    return shown() != NULL || !any_calendar() ? s_view : NULL;
}

/* Local midnight `days` from the day of `t`. */
static int64_t midnight(time_t t, int days)
{
    return (int64_t)nb_midnight(t, days);
}

/* Midnight of today and the next CAL_DAYS days, worked out again only when
 * the day changes (the screen is drawn in strips, each asking). */
static const int64_t *days(time_t now)
{
    static int64_t b[CAL_DAYS + 1];
    if (b[0] == 0 || now < b[0] || now >= b[1]) {
        for (int d = 0; d <= CAL_DAYS; d++) {
            b[d] = midnight(now, d);
        }
    }
    return b;
}

/* Whether event e belongs under the day [ds, de): every event on each day
 * it covers (a conference from Monday to Wednesday under all three), what
 * is over left out. */
static bool on_day(const cal_event_t *e, int64_t ds, int64_t de, time_t now)
{
    if (e->all_day) {
        return e->start < de && e->end > ds;
    }
    const int64_t end = e->end > e->start ? e->end : e->start + 1;
    return end > now && e->start < de && end > ds;
}

/* The time column for a timed event under the day [ds, de): its start,
 * "til 14:00" on the day it ends after starting before, "Hele dagen" on a
 * day it covers whole. */
static void when_text(char *dst, size_t len, const cal_event_t *ev, int64_t ds, int64_t de)
{
    const bool before = ev->start < ds, after = ev->end > de;
    if (before && after) {
        snprintf(dst, len, "Hele dagen");
        return;
    }
    const time_t t = (time_t)(before ? ev->end : ev->start);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(dst, len, before ? "til %02d:%02d" : "%02d:%02d", lt.tm_hour, lt.tm_min);
}

static void day_heading(char *dst, size_t len, int d, int64_t ds)
{
    if (d == 0) {
        snprintf(dst, len, "I dag");
    } else if (d == 1) {
        snprintf(dst, len, "I morgen");
    } else {
        const time_t t = (time_t)ds;
        struct tm lt;
        localtime_r(&t, &lt);
        snprintf(dst, len, "%s %d. %s", NB_WEEKDAY[lt.tm_wday], lt.tm_mday, NB_MONTH[lt.tm_mon]);
        dst[0] = (char)toupper((unsigned char)dst[0]); /* every weekday starts with an ASCII letter */
    }
}

/* How many events are still to come in the window. */
static int upcoming(const cal_list_t *l, time_t now)
{
    int n = 0;
    const int64_t *b = days(now);
    for (int d = 0; d < CAL_DAYS; d++) {
        const int64_t ds = b[d], de = b[d + 1];
        for (int i = 0; i < l->count; i++) {
            n += on_day(&l->ev[i], ds, de, now);
        }
    }
    return n;
}

static void calendar_draw_cb(lv_event_t *e)
{
    const cal_list_t *l = view();
    if (l == NULL) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    const int lh = lv_font_get_line_height(g_font_body);
    const int rh = lh + 6, head_h = lh + 10, bottom = BOARD_LCD_V_RES - 6;
    const int col_w = (BOARD_LCD_H_RES - 2 * CAL_X - CAL_GAP) / 2;
    const int tw = draw_text_w("Hele dagen") + 14;
    const lv_color_t c_txt = text_colour(), c_dim = dim_colour();
    const time_t now = time(NULL);
    int col = 0, x = CAL_X, y = CAL_TOP;
    const int64_t *b = days(now);

    for (int d = 0; d < CAL_DAYS; d++) {
        const int64_t ds = b[d], de = b[d + 1];
        bool headed = false;
        for (int i = 0; i < l->count; i++) {
            const cal_event_t *ev = &l->ev[i];
            if (!on_day(ev, ds, de, now)) {
                continue;
            }
            /* A heading needs room for at least one row under it. */
            const int need = (headed ? 0 : head_h) + rh;
            if (y + need > bottom) {
                if (++col > 1) {
                    return;
                }
                x = CAL_X + col_w + CAL_GAP;
                y = CAL_TOP;
            }
            if (!headed) {
                char h[48];
                day_heading(h, sizeof(h), d, ds);
                draw_text(layer, h, x, y, col_w, LV_TEXT_ALIGN_LEFT, c_dim);
                draw_line(layer, x, y + lh + 3, x + col_w, y + lh + 3, 1, c_dim);
                y += head_h;
                headed = true;
            }
            char when[24];
            if (ev->all_day) {
                snprintf(when, sizeof(when), "Hele dagen");
            } else {
                when_text(when, sizeof(when), ev, ds, de);
            }
            draw_rect(layer, x, y + 2, x + 4, y + lh - 1, 2, lv_color_hex(calendar_feed_colour(ev->feed)));
            draw_text(layer, when, x + 12, y, tw, LV_TEXT_ALIGN_LEFT, c_dim);
            draw_text_fit(layer, ev->title, x + 12 + tw, y, col_w - 12 - tw, c_txt);
            y += rh;
        }
        if (headed) {
            y += 6;
        }
    }
}

/* The calendars' events and the display's own for the days shown, into
 * s_view (adapter lock held). */
static void build_view(void)
{
    const cal_list_t *l = shown();
    s_view->count = 0;
    if (l != NULL) {
        cal_list_copy(s_view, l);
    }
    const time_t now = time(NULL);
    if (now > PLAUSIBLE_EPOCH_S) {
        const int64_t *b = days(now);
        events_days(b[0], b[CAL_DAYS], OWN_ON_CALENDAR, s_view);
    }
    cal_sort(s_view);
}

static int64_t s_shown_day; /* nb_day_number of the day drawn as today */

/* Labels for what is held (adapter lock held). */
static void calendar_show(void)
{
    s_shown_day = nb_day_number(time(NULL));
    build_view();
    const cal_list_t *l = shown();
    if (l != NULL && s_at.when > 0) {
        /* " (kalender 1 og 3 ikke oppdatert)" */
        char missing[48] = "";
        int n = 0, cnt = 0;
        for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
            cnt += (s_feed_failed >> i) & 1;
        }
        for (int i = 0, k = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
            if (s_feed_failed & (1u << i)) {
                k++;
                n += snprintf(missing + n, sizeof(missing) - n, "%s%d",
                              k == 1 ? " (kalender " : (k == cnt ? " og " : ", "), i + 1);
            }
        }
        if (cnt > 0) {
            snprintf(missing + n, sizeof(missing) - n, " ikke oppdatert)");
        }
        updated_label_set(s_updated, s_at.when, s_failed, missing);
        if (s_feed_failed) {
            lv_obj_set_style_text_color(s_updated, lv_color_hex(STALE_COLOUR), 0);
        }
    } else {
        updated_label_set(s_updated, 0, false, NULL);
    }
    if (!any_calendar() && upcoming(s_view, time(NULL)) == 0) {
        lv_label_set_text(g_status_label, events_count() > 0
                                              ? "Ingen hendelser de neste to ukene."
                                              : "Ingen kalender eller hendelser lagt inn.\nLegg dem inn "
                                                "p\xC3\xA5 oppsettsiden.");
    } else if (!any_calendar()) {
        lv_label_set_text(g_status_label, "");
    } else if (l == NULL) {
        lv_label_set_text(g_status_label, "Henter kalenderen...");
    } else if (upcoming(s_view, time(NULL)) == 0) {
        lv_label_set_text(g_status_label, "Ingen avtaler de neste to ukene.");
    } else {
        lv_label_set_text(g_status_label, "");
    }
    lv_obj_invalidate(s_canvas);
}

/* At midnight, while on show: "I dag" and "I morgen" move on (otherwise
 * only at the next poll, minutes later). */
static void calendar_day_timer_cb(lv_timer_t *t)
{
    (void)t;
    const time_t now = time(NULL);
    if (!lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN) && now > PLAUSIBLE_EPOCH_S &&
        nb_day_number(now) != s_shown_day) {
        calendar_show();
    }
}

lv_obj_t *calendar_build(lv_obj_t *screen)
{
    s_root = screen_root_create(screen);
    s_list = cal_list_new(CAL_EVENTS_MAX);
    s_scratch = cal_list_new(CAL_EVENTS_MAX);
    s_view = cal_list_new(CAL_EVENTS_MAX + CAL_OWN_MAX);
    assert(s_list != NULL && s_scratch != NULL && s_view != NULL);

    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, calendar_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, CAL_X, 4);
    lv_label_set_text(s_title, "Kalender");

    s_updated = lv_label_create(s_root);
    lv_obj_align(s_updated, LV_ALIGN_TOP_RIGHT, -CAL_X, 4);
    lv_label_set_text(s_updated, "");
    lv_timer_create(calendar_day_timer_cb, 1000, NULL);
    return s_root;
}

void calendar_enter(void)
{
    s_failed = false;
    calendar_show();
}

uint32_t calendar_poll(int for_view)
{
    const time_t now = time(NULL);
    bool failed = false;
    uint32_t wait = CAL_POLL_MS;
    if (!any_calendar()) {
        wait = CAL_IDLE_MS;
    } else if (now <= PLAUSIBLE_EPOCH_S) {
        return CAL_RETRY_MS; /* the window needs the date */
    } else if (!stamp_fresh(&s_at, CAL_FETCH_MS) ||
               (s_feed_failed && !s_retried && !stamp_fresh(&s_at, CAL_RETRY_MS))) {
        /* Early, once, when a calendar failed last time. */
        const bool quick = stamp_fresh(&s_at, CAL_FETCH_MS);
        const char *urls[APP_CONFIG_CAL_FEEDS];
        for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
            urls[i] = g_cfg->cal_url[i];
        }
        uint8_t feed_failed = 0;
        esp_err_t err = cal_fetch(urls, APP_CONFIG_CAL_FEEDS, midnight(now, 0), midnight(now, CAL_DAYS), s_scratch,
                                  &feed_failed);
        if (err == ESP_OK) {
            /* A calendar that failed keeps the events it had, for as long
             * as a whole list would be shown. */
            uint8_t keep = 0;
            for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
                if (!(feed_failed & (1u << i))) {
                    stamp_now(&s_feed_at[i]);
                } else if (stamp_fresh(&s_feed_at[i], CAL_KEEP_MS)) {
                    keep |= 1u << i;
                }
            }
            cal_keep_feeds(s_scratch, s_list, keep, midnight(now, 0), midnight(now, CAL_DAYS));
            s_retried = feed_failed && quick;
            if (feed_failed) {
                diag_fail(DIAG_CALENDAR, ESP_FAIL);
            } else {
                diag_ok(DIAG_CALENDAR);
            }
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                cal_list_copy(s_list, s_scratch);
                s_feed_failed = feed_failed;
                stamp_now(&s_at);
                esp_lv_adapter_unlock();
            }
            ESP_LOGI(TAG, "%d event(s) in the next %d days", s_scratch->count, CAL_DAYS);
        } else {
            diag_fail(DIAG_CALENDAR, err);
            failed = true;
        }
    }
    bool ok = !any_calendar();
    if (lock_for_view(for_view)) {
        s_failed = failed && shown() != NULL;
        calendar_show();
        ok |= shown() != NULL;
        if (!ok && failed) {
            lv_label_set_text(g_status_label, "Kunne ikke hente kalenderen. Pr\xC3\xB8ver igjen...");
        }
        esp_lv_adapter_unlock();
    }
    if (ok && s_feed_failed && !s_retried) {
        wait = CAL_RETRY_MS;
    }
    return ok ? wait : CAL_RETRY_MS;
}
