
/* Departure board: the next departures per line and direction from the
 * location's stops (entur_client), under a large clock. Painted from one
 * draw handler like the radar, which keeps the row count free of widgets.
 * Polled every 30 s, and repainted between polls so the "N min" countdowns
 * stay current. */

#include "esp_attr.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"

#include "app.h"
#include "diag.h"
#include "departures.h"
#include "draw.h"
#include "entur_client.h"
#include "waveshare_rgb_lcd_port.h"

#define DEP_POLL_MS         30000
#define DEP_RETRY_MS        15000
#define DEP_REDRAW_S        15
#define DEP_CLOCK_PX        48
#define DEP_BODY_Y          64
#define DEP_X               12
#define DEP_BADGE_W         64
#define DEP_TIME_W          112
#define DEP_GONE_S          30  /* a departure this long past is no longer shown */
#define DEP_NOTICE_COLOUR   0xD35400 /* disruption notices */
#define DEP_SHARED_MAX      3        /* notices listed once at the bottom */

/* Departure board (built only if some location has one): the location's
 * name, a large 24-hour clock, and the rows painted by dep_draw_cb from
 * s_dep_data. s_dep_sel is the parsed selection of the location on show. */
static lv_obj_t *s_dep_root;
static lv_obj_t *s_dep_title;
/* The clock, one label per character ("HH:MM:SS") in cells of a fixed
 * width, so the proportional digits don't shift it as the time ticks. */
#define DEP_CLOCK_CHARS 8
static lv_obj_t *s_dep_clock[DEP_CLOCK_CHARS];
static lv_obj_t *s_dep_updated;
static lv_obj_t *s_dep_canvas;
static entur_departures_t *s_dep_data; /* PSRAM */
static entur_selection_t *s_dep_sel;   /* PSRAM */
static bool s_dep_valid;
#define DEP_CACHE_MAX_MS    (5 * 60 * 1000)
static entur_departures_t *s_dep_cache[APP_CONFIG_MAX_LOCATIONS];
static fetch_stamp_t s_dep_at[APP_CONFIG_MAX_LOCATIONS];
static entur_departures_t *s_dep_scratch; /* PSRAM; each fetch lands here first */

static lv_color_t dep_text_color(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0xDDE6EE : 0x1B2631);
}

static lv_color_t dep_dim_color(void)
{
    return lv_color_hex(g_cfg->theme == APP_THEME_DARK ? 0x8AA0B4 : 0x5D6D7E);
}

/* Badge colour for lines without one of their own (Ruter's mode colours). */
static uint32_t dep_mode_colour(const char *mode)
{
    if (strcmp(mode, "bus") == 0 || strcmp(mode, "coach") == 0) return 0xE60000;
    if (strcmp(mode, "tram") == 0) return 0x0B91EF;
    if (strcmp(mode, "metro") == 0) return 0xEC700C;
    if (strcmp(mode, "rail") == 0) return 0x003087;
    if (strcmp(mode, "water") == 0) return 0x682C88;
    return 0x607D8B;
}

/* "Nå" (under a minute), "N min" (under 15), otherwise HH:MM - as the web page. */
static void dep_fmt_time(char *buf, size_t n, int64_t t, time_t now)
{
    const int64_t mins = (t - (int64_t)now) / 60;
    if (mins < 1) {
        snprintf(buf, n, "N\xC3\xA5");
    } else if (mins < 15) {
        snprintf(buf, n, "%d min", (int)mins);
    } else {
        time_t tt = (time_t)t;
        struct tm lt;
        localtime_r(&tt, &lt);
        snprintf(buf, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
}

/* One departure in the time column ending at x_right: red "Innstilt" if
 * cancelled, dimmed if it's only the timetable (no realtime data). */
static void dep_draw_call(lv_layer_t *layer, const entur_call_t *c, int x_right, int y, time_t now)
{
    char buf[16];
    lv_color_t color = dep_text_color();
    if (c->cancelled) {
        snprintf(buf, sizeof(buf), "Innstilt");
        color = lv_palette_main(LV_PALETTE_RED);
    } else {
        dep_fmt_time(buf, sizeof(buf), c->expected, now);
        if (!c->realtime) {
            color = dep_dim_color();
        }
    }
    draw_text(layer, buf, x_right - DEP_TIME_W, y, DEP_TIME_W, LV_TEXT_ALIGN_RIGHT, color);
}

/* Whether call `c` is still to come (or only just gone) at `now` - the data
 * can be up to a poll old, or much older while fetches fail. */
static bool dep_call_upcoming(const entur_call_t *c, time_t now)
{
    const int64_t t = c->cancelled ? c->aimed : c->expected;
    return t >= (int64_t)now - DEP_GONE_S;
}

/* Where `notice` is in shared[0..n), or -1. */
static int dep_shared_index(const char *const *shared, int n, const char *notice)
{
    for (int k = 0; notice[0] && k < n; k++) {
        if (strcmp(shared[k], notice) == 0) {
            return k;
        }
    }
    return -1;
}

/* Notice number k + 1 in an orange dot `d` px across at x, y. */
static void dep_note_mark(lv_layer_t *layer, int x, int y, int d, int k)
{
    draw_dot(layer, x + d / 2, y + d / 2, d / 2, lv_color_hex(DEP_NOTICE_COLOUR));
    const char num[2] = { (char)('1' + k % 9), '\0' };
    const int lh = lv_font_get_line_height(g_font_body);
    draw_text(layer, num, x, y + (d - lh) / 2, d, LV_TEXT_ALIGN_CENTER, lv_color_white());
}

static void dep_draw_cb(lv_event_t *e)
{
    if (!s_dep_valid) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    const int lh = lv_font_get_line_height(g_font_body);
    const int row_h = lh + 12;
    int bottom = EXAMPLE_LCD_V_RES - lh - 8; /* leaves the footnote clear */
    const int x_t2 = EXAMPLE_LCD_H_RES - DEP_X;
    const int x_t1 = x_t2 - DEP_TIME_W - 12;
    const int x_dest = DEP_X + DEP_BADGE_W + 14;
    const time_t now = time(NULL);
    const lv_color_t c_txt = dep_text_color();
    const lv_color_t c_dim = dep_dim_color();

    /* Rows with something still to come, to say how many didn't fit. */
    int rows_left = 0;
    for (int g = 0; g < s_dep_data->group_count; g++) {
        const entur_group_t *grp = &s_dep_data->groups[g];
        for (int c = 0; c < grp->call_count; c++) {
            if (now <= PLAUSIBLE_EPOCH_S || dep_call_upcoming(&grp->calls[c], now)) {
                rows_left++;
                break;
            }
        }
    }

    /* A notice on several rows is listed once at the bottom, and those rows
     * get its number in an orange dot instead; one on a single row stays
     * under that row. */
    const char *shared[DEP_SHARED_MAX];
    int n_shared = 0;
    for (int g = 0; g < s_dep_data->group_count; g++) {
        const char *nt = s_dep_data->groups[g].notice;
        if (nt[0] == '\0' || n_shared == DEP_SHARED_MAX || dep_shared_index(shared, n_shared, nt) >= 0) {
            continue;
        }
        for (int h = g + 1; h < s_dep_data->group_count; h++) {
            if (strcmp(s_dep_data->groups[h].notice, nt) == 0) {
                shared[n_shared++] = nt;
                break;
            }
        }
    }
    /* Up to two lines each. */
    const int note_x = DEP_X + (lh - 4) + 8, note_w = x_t2 - note_x;
    int note_lines[DEP_SHARED_MAX], notes_h = 0;
    for (int k = 0; k < n_shared; k++) {
        note_lines[k] = draw_text_lines(shared[k], note_w, 2);
        notes_h += note_lines[k] * lh + 2;
    }
    bottom -= notes_h;

    int y = DEP_BODY_Y;
    for (int si = 0; si < s_dep_data->stop_count && y + row_h <= bottom; si++) {
        /* The stop's name as a heading - only needed to tell several apart. */
        if (s_dep_data->stop_count > 1) {
            draw_text_fit(layer, s_dep_data->stop_name[si], DEP_X, y + 4, x_t1 - DEP_X, c_dim);
            y += lh + 6;
        }
        bool any = false;
        for (int g = 0; g < s_dep_data->group_count && y + row_h <= bottom; g++) {
            const entur_group_t *grp = &s_dep_data->groups[g];
            if (grp->stop != si) {
                continue;
            }
            /* A row with a disruption notice gets a line under it for it. */
            const int shared_i = dep_shared_index(shared, n_shared, grp->notice);
            const int this_h = row_h + (grp->notice[0] && shared_i < 0 ? lh : 0);
            if (y + this_h > bottom) {
                break;
            }
            /* Only the departures still to come; a row whose departures have all gone
             * is left out until the next poll brings the next ones. */
            const entur_call_t *calls[ENTUR_PER_GROUP];
            int n_calls = 0;
            for (int c = 0; c < grp->call_count; c++) {
                if (now <= PLAUSIBLE_EPOCH_S || dep_call_upcoming(&grp->calls[c], now)) {
                    calls[n_calls++] = &grp->calls[c];
                }
            }
            if (n_calls == 0) {
                continue;
            }
            any = true;

            /* The line number on the line's own colour, as on the web page. */
            lv_area_t badge = { DEP_X, y + 2, DEP_X + DEP_BADGE_W - 1, y + row_h - 3 };
            if (draw_area_visible(layer, &badge)) {
                lv_draw_rect_dsc_t d;
                lv_draw_rect_dsc_init(&d);
                d.radius = 6;
                d.bg_opa = LV_OPA_COVER;
                d.bg_color = lv_color_hex(grp->has_colour ? grp->colour : dep_mode_colour(grp->mode));
                lv_draw_rect(layer, &d, &badge);
            }
            draw_text(layer, grp->code, DEP_X, y + (row_h - lh) / 2, DEP_BADGE_W, LV_TEXT_ALIGN_CENTER,
                       lv_color_hex(grp->has_colour ? grp->text_colour : 0xFFFFFF));

            /* The quay: "(Spor 2)" for trains, none for the metro (its
             * platforms aren't numbered for travellers), "(2)" otherwise. */
            char dest[sizeof(grp->dest) + sizeof(grp->quay) + 16];
            if (grp->quay[0] && strcmp(grp->mode, "rail") == 0) {
                snprintf(dest, sizeof(dest), "%s (Spor %s)", grp->dest, grp->quay);
            } else if (grp->quay[0] && strcmp(grp->mode, "metro") != 0) {
                snprintf(dest, sizeof(dest), "%s (%s)", grp->dest, grp->quay);
            } else {
                snprintf(dest, sizeof(dest), "%s", grp->dest);
            }
            const int dest_w = x_t1 - DEP_TIME_W - x_dest - 8;
            if (shared_i >= 0) {
                /* Room for the notice's number after the destination. */
                const int dot = lh - 4;
                const int text_w = draw_text_w(dest);
                const int fit_w = dest_w - dot - 6;
                draw_text_fit(layer, dest, x_dest, y + (row_h - lh) / 2, fit_w, c_txt);
                dep_note_mark(layer, x_dest + (text_w < fit_w ? text_w : fit_w) + 6, y + (row_h - dot) / 2, dot,
                              shared_i);
            } else {
                draw_text_fit(layer, dest, x_dest, y + (row_h - lh) / 2, dest_w, c_txt);
            }

            dep_draw_call(layer, calls[0], x_t1, y + (row_h - lh) / 2, now);
            if (n_calls > 1) {
                dep_draw_call(layer, calls[1], x_t2, y + (row_h - lh) / 2, now);
            }
            if (grp->notice[0] && shared_i < 0) {
                draw_text_fit(layer, grp->notice, x_dest, y + row_h - 6, x_t2 - x_dest,
                              lv_color_hex(DEP_NOTICE_COLOUR));
            }
            y += this_h;
            rows_left--;
        }
        if (!any && y + row_h <= bottom) {
            draw_text(layer, "Ingen avganger de neste 24 timene.", x_dest, y + (row_h - lh) / 2,
                       x_t2 - x_dest, LV_TEXT_ALIGN_LEFT, c_dim);
            y += row_h;
        }
        y += 6;
    }
    /* The shared notices, once each, above the bottom line. */
    int ny = bottom + 4;
    for (int k = 0; k < n_shared; k++) {
        const int dot = lh - 4;
        dep_note_mark(layer, DEP_X, ny + (lh - dot) / 2, dot, k);
        draw_text_wrap(layer, shared[k], note_x, ny, note_w, note_lines[k], lv_color_hex(DEP_NOTICE_COLOUR));
        ny += note_lines[k] * lh + 2;
    }
    bottom += notes_h;

    if (rows_left > 0) {
        char more[64];
        /* At ENTUR_MAX_GROUPS the fetch itself may have left some out. */
        snprintf(more, sizeof(more), "+ %s%d %s som ikke f\xC3\xA5r plass",
                 s_dep_data->group_count >= ENTUR_MAX_GROUPS ? "minst " : "", rows_left,
                 rows_left == 1 ? "linje" : "linjer");
        draw_text(layer, more, DEP_X, bottom + 4, x_t1 - DEP_X, LV_TEXT_ALIGN_LEFT, c_dim);
    }
}

/* Every second while the board is on show: the clock, and every
 * DEP_REDRAW_S a repaint so "N min" counts down between polls. */
static void dep_clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    static time_t last_redraw;
    if (lv_obj_has_flag(s_dep_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    time_t now = time(NULL);
    char txt[DEP_CLOCK_CHARS + 1] = "--:--:--";
    struct tm lt = { 0 };
    if (now > PLAUSIBLE_EPOCH_S) {
        localtime_r(&now, &lt);
        snprintf(txt, sizeof(txt), "%02u:%02u:%02u", (unsigned)lt.tm_hour % 100u, (unsigned)lt.tm_min % 100u,
                 (unsigned)lt.tm_sec % 100u);
    }
    for (int i = 0; i < DEP_CLOCK_CHARS; i++) {
        const char *cur = lv_label_get_text(s_dep_clock[i]);
        if (cur[0] != txt[i]) {
            char ch[2] = { txt[i], '\0' };
            lv_label_set_text(s_dep_clock[i], ch);
        }
    }
    if (now <= PLAUSIBLE_EPOCH_S) {
        return;
    }
    if (s_dep_valid && (now - last_redraw >= DEP_REDRAW_S || lt.tm_sec == 0)) {
        last_redraw = now;
        lv_obj_invalidate(s_dep_canvas);
    }
}

lv_obj_t *departures_build(lv_obj_t *screen)
{
    s_dep_data = heap_caps_calloc(1, sizeof(*s_dep_data), MALLOC_CAP_SPIRAM);
    s_dep_sel = heap_caps_calloc(1, sizeof(*s_dep_sel), MALLOC_CAP_SPIRAM);
    s_dep_scratch = heap_caps_malloc(sizeof(*s_dep_scratch), MALLOC_CAP_SPIRAM);
    assert(s_dep_data != NULL && s_dep_sel != NULL && s_dep_scratch != NULL);
    for (int i = 0; i < g_cfg->location_count; i++) {
        if (g_cfg->show[i] & APP_SHOW_DEPARTURES) {
            s_dep_cache[i] = heap_caps_malloc(sizeof(entur_departures_t), MALLOC_CAP_SPIRAM);
        }
    }
    lv_obj_t *root = s_dep_root = screen_root_create(screen);

    s_dep_canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s_dep_canvas);
    lv_obj_set_pos(s_dep_canvas, 0, 0);
    lv_obj_set_size(s_dep_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_dep_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dep_canvas, dep_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_dep_title = lv_label_create(root);
    lv_obj_set_style_text_font(s_dep_title, g_font_large, 0);
    lv_obj_set_pos(s_dep_title, DEP_X, 4);
    lv_obj_set_width(s_dep_title, 480);
    lv_label_set_long_mode(s_dep_title, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s_dep_title, "");

    /* The clock gets a font of its own, big enough to read across a room. */
    /* Each digit gets a cell as wide as the widest digit, each colon one as
     * wide as a colon; the characters are centred in their cells. */
    const lv_font_t *clock_font = load_font(DEP_CLOCK_PX, true);
    int digit_w = 0;
    for (uint32_t c = '0'; c <= '9'; c++) {
        int w = lv_font_get_glyph_width(clock_font, c, 0);
        digit_w = w > digit_w ? w : digit_w;
    }
    const int colon_w = lv_font_get_glyph_width(clock_font, ':', 0) + 4;
    lv_obj_t *clock = lv_obj_create(root);
    lv_obj_remove_style_all(clock);
    lv_obj_clear_flag(clock, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(clock, clock_font, 0);
    lv_obj_set_style_text_align(clock, LV_TEXT_ALIGN_CENTER, 0);
    int x = 0;
    for (int i = 0; i < DEP_CLOCK_CHARS; i++) {
        const int w = (i == 2 || i == 5) ? colon_w : digit_w;
        s_dep_clock[i] = lv_label_create(clock);
        lv_obj_set_pos(s_dep_clock[i], x, 0);
        lv_obj_set_width(s_dep_clock[i], w);
        lv_label_set_text(s_dep_clock[i], (i == 2 || i == 5) ? ":" : "-");
        x += w;
    }
    lv_obj_set_size(clock, x, lv_font_get_line_height(clock_font));
    lv_obj_align(clock, LV_ALIGN_TOP_RIGHT, -DEP_X, 0);

    s_dep_updated = lv_label_create(root);
    lv_obj_set_style_text_color(s_dep_updated, dep_dim_color(), 0);
    lv_obj_align(s_dep_updated, LV_ALIGN_BOTTOM_RIGHT, -DEP_X, -4);
    lv_label_set_text(s_dep_updated, "");

    lv_timer_create(dep_clock_timer_cb, 1000, NULL);

    return root;
}

/* "Oppdatert HH:MM:SS" for data fetched at `at` (adapter lock held). */
static void dep_updated_set(const fetch_stamp_t *at)
{
    if (at->when > 0) {
        struct tm lt;
        localtime_r(&at->when, &lt);
        lv_label_set_text_fmt(s_dep_updated, "Oppdatert %02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    } else {
        lv_label_set_text(s_dep_updated, "");
    }
}

/* Point the board at location `loc` (adapter lock held): its title and its
 * selection, and its last departures if they're recent - otherwise blank
 * until the first fetch for it lands. */
void departures_enter(int loc)
{
    if (!entur_parse_selection(g_cfg->departures[loc], s_dep_sel)) {
        s_dep_sel->stop_count = 0;
    }
    lv_label_set_text_fmt(s_dep_title, "Avganger %s", g_cfg->locations[loc].name);
    s_dep_valid = s_dep_cache[loc] != NULL && stamp_fresh(&s_dep_at[loc], DEP_CACHE_MAX_MS);
    if (s_dep_valid) {
        memcpy(s_dep_data, s_dep_cache[loc], sizeof(*s_dep_data));
        dep_updated_set(&s_dep_at[loc]);
    } else {
        lv_label_set_text(s_dep_updated, "");
    }
    lv_obj_invalidate(s_dep_canvas);
    lv_label_set_text(g_status_label, s_dep_valid ? "" : "Henter avganger...");
}

/* Fetch `loc`'s departures and show them if the screen is still `for_view`.
 * Returns how long to wait before the next poll. */
uint32_t departures_poll(int loc, int for_view)
{
    /* s_dep_sel was parsed for `loc` by departures_enter; copied, as a tap
     * may re-parse it for another location while the fetch is under way. */
    static EXT_RAM_BSS_ATTR entur_selection_t sel;
    bool have = false;
    if (lock_for_view(for_view)) {
        sel = *s_dep_sel;
        have = true;
        if (sel.stop_count == 0) {
            lv_label_set_text(g_status_label, "Ingen holdeplasser valgt.\nLegg dem inn p\xC3\xA5 oppsettsiden.");
        }
        esp_lv_adapter_unlock();
    }
    if (!have || sel.stop_count == 0) {
        return DEP_RETRY_MS;
    }
    esp_err_t err = entur_client_fetch(&sel, s_dep_scratch);
    if (err == ESP_OK) {
        diag_ok(DIAG_DEPARTURES);
    } else {
        diag_fail(DIAG_DEPARTURES, err);
    }

    if (!lock_for_view(for_view)) {
        return DEP_RETRY_MS;
    }
    if (err == ESP_OK) {
        memcpy(s_dep_data, s_dep_scratch, sizeof(*s_dep_data));
        s_dep_valid = true;
        stamp_now(&s_dep_at[loc]);
        if (s_dep_cache[loc] != NULL) {
            memcpy(s_dep_cache[loc], s_dep_scratch, sizeof(*s_dep_scratch));
        }
        lv_label_set_text(g_status_label, "");
        lv_obj_set_style_text_color(s_dep_updated, dep_dim_color(), 0);
        dep_updated_set(&s_dep_at[loc]);
        lv_obj_invalidate(s_dep_canvas);
    } else if (!s_dep_valid) {
        lv_label_set_text(g_status_label, "Kunne ikke hente avganger. Pr\xC3\xB8ver igjen...");
    } else {
        lv_obj_set_style_text_color(s_dep_updated, lv_color_hex(0xE07000), 0);
        lv_label_set_text(s_dep_updated, "Kunne ikke oppdatere - viser siste data");
    }
    const bool shown = s_dep_valid;
    esp_lv_adapter_unlock();
    return shown ? DEP_POLL_MS : DEP_RETRY_MS;
}
