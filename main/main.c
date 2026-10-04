/* Start-up, the cycle of screens (taps and the automatic rotation), and the
 * weather task that does all the fetching for whichever screen is shown.
 * The screens themselves are in weather.c (a location's forecast and the
 * overview), week.c, radar.c (aircraft, ships and rain), departures.c,
 * air.c, tide.c and calendar.c. */

#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmap_generate_fonts.h"
#include "nvs_flash.h"
#include "adsb_client.h"
#include "ais_client.h"
#include "app.h"
#include "departures.h"
#include "air.h"
#include "calendar.h"
#include "tide.h"
#include "week.h"
#include "entur_client.h"
#include "radar.h"
#include "rain_client.h"
#include "screenshot.h"
#include "watchdog.h"
#include "updater.h"
#include "diag.h"
#include "clock.h"
#include "waveshare_rgb_lcd_port.h"
#include "weather.h"
#include "wifi_provision.h"
#include "yr_client.h"
#include "restart.h"
#include "cJSON.h"

static const char *TAG = "main";

#define YR_TASK_STACK_SIZE 8192

/* Restart once a day, at this local hour, purely as memory-pressure
 * housekeeping (a fresh boot resets any accumulated heap fragmentation).
 * Needs the wall clock to actually be synced (see the SNTP setup below) -
 * without it "now" would read as the 1970 epoch, so scheduling is skipped
 * until a sync has happened. The current screen survives the restart (see
 * app_config_save_last_view). */
#define NIGHTLY_REBOOT_HOUR 2

/* Night dimming: the backlight itself can't be dimmed (see s_tap_layer), so
 * a translucent black layer over the whole screen stands in for it during
 * the local hours set on the setup page (g_cfg->dim_*). LV_OPA_70 cuts the
 * effective brightness a lot while keeping high-contrast text/lines legible
 * in a dark room. */
#define NIGHT_DIM_OPA         LV_OPA_70

/* With the screen switched off at night instead (g_cfg->night_off), a touch
 * lights it for this long. */
#define NIGHT_WAKE_MS         60000

/* Warn on screen if the clock still isn't set this long after boot. */
#define CLOCK_WARN_US         (10 * 60 * 1000000LL)

/* How long the weather task lets a screen just switched to draw before it
 * starts fetching for it: drawing a full screen and a TLS fetch at once took
 * internal DRAM down to a few KB, where WiFi's own buffers start failing. */
#define VIEW_SETTLE_MS      500

/* A screen chosen by tap is remembered for the next boot once it has been
 * on show this long - not on every tap, each of which would write flash. */
#define LAST_VIEW_SAVE_MS   10000

app_config_t *g_cfg;
const lv_font_t *g_font_body;
const lv_font_t *g_font_large;
lv_obj_t *g_status_label;

/* The screens a tap cycles through, in order (see build_stops): the overview
 * table and the calendar if shown, then for each location whichever of its
 * weather, week, aircraft, ships, rain, departures, air and tide screens are
 * enabled, in that order. */
typedef struct {
    uint8_t kind; /* stop_kind_t */
    uint8_t loc;  /* location index; unused for the overview */
} view_stop_t;
static view_stop_t s_stops[2 + 8 * APP_CONFIG_MAX_LOCATIONS]; /* overview, calendar, up to eight per location */
static int s_stop_count;
static bool s_any_radar;      /* some location shows aircraft, ships or rain (they share the radar screen) */
static bool s_any_departures; /* some location shows a departure board */
static bool s_any_air;        /* some location shows the air screen */
static bool s_any_week;       /* some location shows the week screen */
static bool s_any_tide;       /* some location shows the tide screen */

/* Index into s_stops of the screen on show. Switched by a tap or the
 * automatic rotation (view_enter); the weather task watches it and fetches.
 * s_view_auto says the rotation made the last switch. */
volatile int g_view_index;
static volatile bool s_view_auto;

/* Automatic rotation (see auto_rotate_timer_cb): when the screen was last
 * touched, and whether the rotation has moved it since. */
static uint32_t s_last_touch_ms;
static bool s_auto_running;
static uint32_t s_auto_switch_ms;
static TaskHandle_t s_yr_task;

/* One full-screen container per kind of screen; only one is visible. */
static lv_obj_t *s_detail_root;   /* a location's weather */
static lv_obj_t *s_overview_root; /* all locations' weather */
static lv_obj_t *s_radar_root;    /* aircraft, ships or rain; only if some location has one */
static lv_obj_t *s_dep_root;      /* departures; only if some location has them */
static lv_obj_t *s_air_root;      /* air quality and pollen; likewise */
static lv_obj_t *s_week_root;     /* the week ahead; likewise */
static lv_obj_t *s_tide_root;     /* tides; likewise */
static lv_obj_t *s_cal_root;      /* the calendar; only if shown */
static lv_obj_t *s_tap_layer;     /* full-screen tap catcher; also the night-dim overlay */
static volatile bool s_night_dim; /* inside the night window (see nightly_housekeeping) */
static bool s_backlight_on = true;
static lv_obj_t *s_offline_label; /* "Ingen WiFi" / "Ingen internett" in a corner */
static void night_timer_cb(lv_timer_t *t);

bool lock_for_view(int for_view)
{
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return false;
    }
    if (g_view_index != for_view) {
        esp_lv_adapter_unlock();
        return false;
    }
    return true;
}

lv_obj_t *screen_root_create(lv_obj_t *screen)
{
    lv_obj_t *root = lv_obj_create(screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}

const lv_font_t *load_font(uint8_t px, bool bold)
{
    esp_lv_adapter_ft_font_handle_t handle = NULL;
    const esp_lv_adapter_ft_font_config_t cfg = ESP_LV_ADAPTER_FT_FONT_FILE_CONFIG(
        bold ? "F:MontserratBold.ttf" : "F:MontserratMedium.ttf", px, ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL);
    ESP_ERROR_CHECK(esp_lv_adapter_ft_font_init(&cfg, &handle));
    const lv_font_t *font = esp_lv_adapter_ft_font_get(handle);
    assert(font != NULL);
    return font;
}

static void init_fonts(void)
{
    /* Mount the "fonts" SPIFFS partition (built by spiffs_create_partition_assets
     * in main/CMakeLists.txt) as the "F:" drive: LVGL's FreeType binding opens
     * the .ttf by path, and the MET weather icons are loaded the same way
     * (F:<symbol_code>.png, decoded by esp_lv_decoder).
     *
     * Read through esp_partition_read, not memory-mapped: with the app
     * running from PSRAM (SPIRAM_XIP_FROM_PSRAM) the other core keeps going
     * during a flash write - an NVS save, a firmware update - and a glyph
     * read from mapped flash then got garbage, FreeType failed, and LVGL
     * asserted. Partition reads wait for the write instead. */
    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "fonts",
        .max_files = MMAP_FONTS_FILES,
        .checksum = MMAP_FONTS_CHECKSUM,
        .flags = { .mmap_enable = 0 },
    };
    mmap_assets_handle_t mmap_handle = NULL;
    ESP_ERROR_CHECK(mmap_assets_new(&mmap_cfg, &mmap_handle));

    size_t file_count = mmap_assets_get_stored_files(mmap_handle);
    assert(file_count > 0);

    const fs_cfg_t fs_cfg = {
        .fs_letter = 'F',
        .fs_nums = (int)file_count,
        .fs_assets = mmap_handle,
    };
    esp_lv_fs_handle_t fs_handle = NULL;
    ESP_ERROR_CHECK(esp_lv_adapter_fs_mount(&fs_cfg, &fs_handle));

    /* Sizes and weights from the setup page (g_cfg->title_* / text_*). Bold
     * is a font file of its own: LVGL's FreeType binding renders bitmaps, and
     * only its outline mode honours the BOLD style flag. */
    g_font_body = load_font(g_cfg->text_px, g_cfg->text_bold);
    g_font_large = load_font(g_cfg->title_px, g_cfg->title_bold);
}

/* Lay out the cycle of screens from the configuration. */
static void build_stops(void)
{
    s_stop_count = 0;
    if (g_cfg->ov_show) {
        s_stops[s_stop_count++] = (view_stop_t){ STOP_OVERVIEW, 0 };
    }
    if (g_cfg->cal_show) {
        s_stops[s_stop_count++] = (view_stop_t){ STOP_CALENDAR, 0 };
    }
    static const struct {
        uint8_t show, kind;
    } order[] = {
        { APP_SHOW_WEATHER, STOP_WEATHER }, { APP_SHOW_WEEK, STOP_WEEK }, { APP_SHOW_RADAR, STOP_RADAR }, { APP_SHOW_SHIPS, STOP_SHIPS },
        { APP_SHOW_RAIN, STOP_RAIN }, { APP_SHOW_DEPARTURES, STOP_DEPARTURES }, { APP_SHOW_AIR, STOP_AIR },
        { APP_SHOW_TIDE, STOP_TIDE },
    };
    for (int i = 0; i < g_cfg->location_count; i++) {
        for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
            if (g_cfg->show[i] & order[k].show) {
                s_stops[s_stop_count++] = (view_stop_t){ order[k].kind, (uint8_t)i };
            }
        }
        if (g_cfg->show[i] & (APP_SHOW_RADAR | APP_SHOW_SHIPS | APP_SHOW_RAIN)) {
            s_any_radar = true;
        }
        if (g_cfg->show[i] & APP_SHOW_DEPARTURES) {
            s_any_departures = true;
        }
        if (g_cfg->show[i] & APP_SHOW_AIR) {
            s_any_air = true;
        }
        if (g_cfg->show[i] & APP_SHOW_WEEK) {
            s_any_week = true;
        }
        if (g_cfg->show[i] & APP_SHOW_TIDE) {
            s_any_tide = true;
        }
    }
    /* Every location shows at least one screen; just in case, never none. */
    if (s_stop_count == 0) {
        s_stops[s_stop_count++] = (view_stop_t){ STOP_OVERVIEW, 0 };
    }
}

/* Show the container for screens of `kind`, hiding the others. The status
 * label and tap layer sit above all of them and are left alone. */
static void show_view(stop_kind_t kind)
{
    lv_obj_t *const roots[] = { s_overview_root, s_detail_root, s_radar_root, s_dep_root, s_air_root, s_week_root,
                                s_tide_root, s_cal_root };
    lv_obj_t *shown = (kind == STOP_OVERVIEW) ? s_overview_root
                    : (kind == STOP_WEATHER) ? s_detail_root
                    : (kind == STOP_DEPARTURES) ? s_dep_root
                    : (kind == STOP_AIR) ? s_air_root
                    : (kind == STOP_WEEK) ? s_week_root
                    : (kind == STOP_TIDE) ? s_tide_root
                    : (kind == STOP_CALENDAR) ? s_cal_root : s_radar_root;
    for (size_t k = 0; k < sizeof(roots) / sizeof(roots[0]); k++) {
        if (roots[k] == NULL) {
            continue;
        }
        if (roots[k] == shown) {
            lv_obj_clear_flag(roots[k], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(roots[k], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* The status text sits above whichever screen is shown. On the radar it
     * takes the radar's text colour, and is moved over the table half so it
     * doesn't sit on the plot. */
    if (kind == STOP_RADAR || kind == STOP_SHIPS || kind == STOP_RAIN) {
        lv_obj_set_style_text_color(g_status_label, lv_color_hex(radar_status_colour()), 0);
        lv_obj_align(g_status_label, LV_ALIGN_CENTER, 212, 0); /* over the table half */
    } else {
        lv_obj_remove_local_style_prop(g_status_label, LV_STYLE_TEXT_COLOR, 0);
        lv_obj_align(g_status_label, LV_ALIGN_CENTER, 0, 0);
    }
}

/* Switch to screen `idx` of s_stops (adapter lock held), showing whatever
 * is cached for it straight away, or a "Henter..." note until the weather
 * task's fetch lands. Called from a tap, the rotation, and the start. */
static void view_enter(int idx)
{
    const view_stop_t *stop = &s_stops[idx];
    show_view((stop_kind_t)stop->kind);
    switch ((stop_kind_t)stop->kind) {
    case STOP_OVERVIEW:
        overview_enter();
        break;
    case STOP_WEATHER:
        weather_enter(stop->loc);
        break;
    case STOP_RADAR:
    case STOP_SHIPS:
    case STOP_RAIN:
        radar_enter(stop->kind, stop->loc);
        break;
    case STOP_DEPARTURES:
        departures_enter(stop->loc);
        break;
    case STOP_AIR:
        air_enter(stop->loc);
        break;
    case STOP_WEEK:
        week_enter(stop->loc);
        break;
    case STOP_TIDE:
        tide_enter(stop->loc);
        break;
    case STOP_CALENDAR:
        calendar_enter();
        break;
    }
}

/* Switch to stop `next` and wake the weather task to fetch for it. LVGL
 * context (adapter lock held). */
static void view_switch(int next, bool automatic)
{
    s_view_auto = automatic;
    g_view_index = next;
    view_enter(next);
    ESP_LOGI(TAG, "%s: view %d/%d", automatic ? "Auto" : "Tap", next, s_stop_count);
    if (s_yr_task != NULL) {
        xTaskNotifyGive(s_yr_task);
    }
}

/* Tap the right half of the screen: next stop (overview -> location 1 ->
 * location 2 -> ... -> overview); tap the left half: previous stop. Runs in
 * the LVGL context, which already holds the adapter lock. */
static void screen_touch_cb(lv_event_t *e)
{
    /* Any touch holds off the automatic rotation for another idle period. */
    s_last_touch_ms = lv_tick_get();
    s_auto_running = false;

    /* Switched off for the night: a touch only lights it (see night_timer_cb). */
    if (!s_backlight_on) {
        s_backlight_on = (waveshare_rgb_lcd_backlight_set(true) == ESP_OK);
        return;
    }

    if (s_stop_count <= 1) {
        return; /* nothing to cycle through */
    }

    /* Ignore a second press within 500 ms - covers finger bounce and keeps a
     * quick double-tap from skipping two stops by accident. */
    static uint32_t last_tap_ms;
    uint32_t now_ms = lv_tick_get();
    if (now_ms - last_tap_ms < 500) {
        return;
    }
    last_tap_ms = now_ms;

    lv_point_t p = { 0, 0 };
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL) {
        lv_indev_get_point(indev, &p);
    }
    bool left = p.x < lv_obj_get_width(lv_event_get_target_obj(e)) / 2;
    view_switch((g_view_index + (left ? s_stop_count - 1 : 1)) % s_stop_count, false);
}

/* Whether stop `i` is one the automatic rotation visits. */
static bool stop_in_rotation(int i)
{
    static const uint8_t bit[] = {
        [STOP_WEATHER] = APP_SHOW_WEATHER, [STOP_RADAR] = APP_SHOW_RADAR, [STOP_SHIPS] = APP_SHOW_SHIPS,
        [STOP_RAIN] = APP_SHOW_RAIN, [STOP_DEPARTURES] = APP_SHOW_DEPARTURES, [STOP_AIR] = APP_SHOW_AIR,
        [STOP_WEEK] = APP_SHOW_WEEK, [STOP_TIDE] = APP_SHOW_TIDE,
    };
    const view_stop_t *st = &s_stops[i];
    if (st->kind == STOP_OVERVIEW) {
        return g_cfg->auto_overview;
    }
    if (st->kind == STOP_CALENDAR) {
        return g_cfg->cal_rotate;
    }
    return (g_cfg->auto_show[st->loc] & bit[st->kind]) != 0;
}

/* Once a second: after auto_idle_min minutes without a touch, move on to the
 * next screen in the rotation, then again every auto_dwell_s seconds until
 * the next touch - except while the screen is dimmed for the night, if so
 * set. Runs in the LVGL context, like screen_touch_cb. */
static void auto_rotate_timer_cb(lv_timer_t *t)
{
    (void)t;
    const uint32_t now = lv_tick_get();
    if (now - s_last_touch_ms < (uint32_t)g_cfg->auto_idle_min * 60000u) {
        return;
    }
    /* Nobody's watching at night: stay put (and fetch for one screen only). */
    if (s_night_dim && g_cfg->auto_night_pause) {
        return;
    }
    if (s_auto_running && now - s_auto_switch_ms < (uint32_t)g_cfg->auto_dwell_s * 1000u) {
        return;
    }
    s_auto_running = true;
    s_auto_switch_ms = now;
    for (int k = 1; k <= s_stop_count; k++) {
        int next = (g_view_index + k) % s_stop_count;
        if (stop_in_rotation(next)) {
            if (next != g_view_index) {
                view_switch(next, true);
            }
            return;
        }
    }
}

/* /screen, when allowed on the setup page (g_cfg->screen_ctl): with no
 * query, the screens a tap cycles through and which is on show; with
 * ?vis=N (1-based, as listed) or ?sted=N&type=tidevann, switch to that one
 * as a tap would, so /screen.png can capture it. */
static const char *const STOP_NAMES[] = {
    [STOP_OVERVIEW] = "oversikt", [STOP_WEATHER] = "vaer", [STOP_RADAR] = "fly", [STOP_SHIPS] = "skip",
    [STOP_RAIN] = "nedbor", [STOP_DEPARTURES] = "avganger", [STOP_AIR] = "luft", [STOP_WEEK] = "uke",
    [STOP_TIDE] = "tidevann", [STOP_CALENDAR] = "kalender",
};

static esp_err_t screen_send_list(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "vises", g_view_index + 1);
    cJSON *list = cJSON_AddArrayToObject(root, "skjermer");
    for (int i = 0; i < s_stop_count; i++) {
        const view_stop_t *st = &s_stops[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "vis", i + 1);
        cJSON_AddStringToObject(o, "type", STOP_NAMES[st->kind]);
        if (st->kind != STOP_OVERVIEW && st->kind != STOP_CALENDAR) {
            cJSON_AddNumberToObject(o, "sted", st->loc + 1);
            cJSON_AddStringToObject(o, "navn", g_cfg->locations[st->loc].name);
        }
        cJSON_AddItemToArray(list, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t screen_handler(httpd_req_t *req)
{
    if (!g_cfg->screen_ctl) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Valg av skjerm over nettet er sl\xC3\xA5tt av p\xC3\xA5 oppsettsiden "
                                       "(Vedlikehold > Skjermbilder).\n");
    }
    char q[64], val[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return screen_send_list(req);
    }
    int want = -1;
    if (httpd_query_key_value(q, "vis", val, sizeof(val)) == ESP_OK) {
        const int n = atoi(val);
        if (n >= 1 && n <= s_stop_count) {
            want = n - 1;
        }
    } else if (httpd_query_key_value(q, "type", val, sizeof(val)) == ESP_OK) {
        char loc[8] = "1";
        httpd_query_key_value(q, "sted", loc, sizeof(loc));
        const int l = atoi(loc) - 1;
        for (int i = 0; i < s_stop_count && want < 0; i++) {
            const view_stop_t *st = &s_stops[i];
            const bool global = (st->kind == STOP_OVERVIEW || st->kind == STOP_CALENDAR);
            if (strcmp(STOP_NAMES[st->kind], val) == 0 && (global || st->loc == l)) {
                want = i;
            }
        }
    }
    if (want < 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Ingen slik skjerm - se /screen for listen.\n");
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        /* As a tap: hold off the rotation, and light a screen switched off for the night. */
        s_last_touch_ms = lv_tick_get();
        s_auto_running = false;
        if (!s_backlight_on) {
            s_backlight_on = (waveshare_rgb_lcd_backlight_set(true) == ESP_OK);
        }
        if (want != g_view_index) {
            view_switch(want, false);
        }
        esp_lv_adapter_unlock();
    }
    return screen_send_list(req);
}

static void build_ui(lv_obj_t *screen)
{
    const bool dark = (g_cfg->theme == APP_THEME_DARK);
    lv_display_t *disp = lv_obj_get_display(screen);
    lv_display_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                                                     lv_palette_main(LV_PALETTE_RED), dark, g_font_body));
    lv_theme_apply(screen);

    lv_obj_set_style_text_font(screen, g_font_body, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* One container per kind of screen; the radar screen and the departure
     * board exist only if some location has them. */
    s_detail_root = weather_build(screen);
    s_overview_root = overview_build(screen);
    if (s_any_radar) {
        s_radar_root = radar_build(screen);
    }
    if (s_any_departures) {
        s_dep_root = departures_build(screen);
    }
    if (s_any_air) {
        s_air_root = air_build(screen);
    }
    if (s_any_week) {
        s_week_root = week_build(screen);
    }
    if (s_any_tide) {
        s_tide_root = tide_build(screen);
    }
    if (g_cfg->cal_show) {
        s_cal_root = calendar_build(screen);
    }

    /* Created after the screens so it sits on top of whichever is shown:
     * loading/error text, centered over the screen until data lands. */
    g_status_label = lv_label_create(screen);
    lv_obj_align(g_status_label, LV_ALIGN_CENTER, 0, 0);

    /* Offline marker, bottom left (see night_timer_cb). LVGL's built-in
     * Montserrat, for its WiFi and warning symbols. */
    s_offline_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_offline_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_offline_label, lv_color_hex(0xE07000), 0);
    lv_obj_set_style_bg_color(s_offline_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_offline_label, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(s_offline_label, 6, 0);
    lv_obj_set_style_pad_ver(s_offline_label, 2, 0);
    lv_obj_set_style_radius(s_offline_label, 4, 0);
    lv_obj_align(s_offline_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    lv_obj_add_flag(s_offline_label, LV_OBJ_FLAG_HIDDEN);

    /* Full-screen tap catcher. In LVGL 9 every lv_obj/lv_chart is clickable by
     * default, so a tap lands on whichever chart or row widget covers that
     * point and never reaches the screen. This overlay is the topmost child,
     * so it catches every tap anywhere on screen. It doubles as the night
     * dimming overlay (see nightly_housekeeping): the backlight on this board
     * is switched on/off through an I2C GPIO expander with no PWM output, so
     * it can't be dimmed in hardware - a translucent black layer over the
     * content is the closest software equivalent. Transparent (invisible) by
     * default; starts black-with-opacity, so no style is set here. */
    s_tap_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_tap_layer);
    lv_obj_set_pos(s_tap_layer, 0, 0);
    lv_obj_set_size(s_tap_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_tap_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_tap_layer, LV_OBJ_FLAG_SCROLLABLE);
    /* PRESSED (not CLICKED): fires on touch-down regardless of tiny finger
     * movement, so a quick tap is never lost to scroll/gesture detection. */
    lv_obj_add_event_cb(s_tap_layer, screen_touch_cb, LV_EVENT_PRESSED, NULL);

    lv_timer_create(night_timer_cb, 1000, NULL);

    /* Automatic rotation, counting the idle time from boot. */
    s_last_touch_ms = lv_tick_get();
    if (g_cfg->auto_idle_min > 0) {
        lv_timer_create(auto_rotate_timer_cb, 1000, NULL);
    }

    /* Start on whichever screen g_view_index was restored to (the first stop
     * unless another was showing before the previous reboot). */
    view_enter(g_view_index);
    lv_label_set_text(g_status_label, "Kobler til WiFi...");
}

/* Shown on the status label while wifi_provision works (connecting, or the
 * setup-portal instructions). */
static void provision_status_cb(const char *msg)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_label_set_text(g_status_label, msg);
        esp_lv_adapter_unlock();
    }
}

/* The next local NIGHTLY_REBOOT_HOUR:00:00 at or after `now` - today's if it
 * hasn't happened yet, otherwise tomorrow's. `now` must already be a plausible
 * (synced) epoch. */
static time_t compute_next_nightly_reboot(time_t now)
{
    struct tm local;
    localtime_r(&now, &local);
    local.tm_hour = NIGHTLY_REBOOT_HOUR;
    local.tm_min = 0;
    local.tm_sec = 0;
    time_t target = mktime(&local); /* re-normalizes via the TZ set in app_main */
    if (target <= now) {
        target += 24 * 3600;
    }
    return target;
}

/* The night window's look (adapter lock held): the dimming layer, or the
 * backlight off. */
static void night_apply(void)
{
    const bool dim = s_night_dim && !g_cfg->night_off;
    if (dim) {
        lv_obj_set_style_bg_color(s_tap_layer, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_tap_layer, NIGHT_DIM_OPA, 0);
    } else {
        lv_obj_set_style_bg_opa(s_tap_layer, LV_OPA_TRANSP, 0);
    }
    const bool light = !(s_night_dim && g_cfg->night_off);
    if (light != s_backlight_on && waveshare_rgb_lcd_backlight_set(light) == ESP_OK) {
        s_backlight_on = light;
    }
}

/* Once a second, in the LVGL task: switch the screen off again a while
 * after a touch lit it at night, and show whether the display is online. */
static void night_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_night_dim && g_cfg->night_off && s_backlight_on &&
        lv_tick_get() - s_last_touch_ms > NIGHT_WAKE_MS &&
        waveshare_rgb_lcd_backlight_set(false) == ESP_OK) {
        s_backlight_on = false;
    }
    const diag_net_t net = diag_net_state();
    if (net != DIAG_ONLINE) {
        lv_label_set_text(s_offline_label, net == DIAG_NO_WIFI ? LV_SYMBOL_WIFI " Ingen WiFi"
                                                                : LV_SYMBOL_WARNING " Ingen internett");
        lv_obj_clear_flag(s_offline_label, LV_OBJ_FLAG_HIDDEN);
    } else if (!clock_is_set() && esp_timer_get_time() > CLOCK_WARN_US) {
        /* Built-in font: no "ø", hence the plain spelling. */
        lv_label_set_text(s_offline_label, LV_SYMBOL_WARNING " Klokken er ikke stilt");
        lv_obj_clear_flag(s_offline_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_offline_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The nightly restart and the night dimming, checked on every wake of the
 * weather task (every few minutes at idle, immediately on a tap) rather
 * than on timers of their own - a few minutes of drift doesn't matter for
 * either. */
static void nightly_housekeeping(void)
{
    static time_t next_nightly_reboot;  /* 0 = not yet scheduled (clock not synced) */
    time_t now_wall = time(NULL);
    if (now_wall <= PLAUSIBLE_EPOCH_S) {
        return;
    }
    if (next_nightly_reboot == 0) {
        next_nightly_reboot = compute_next_nightly_reboot(now_wall);
        struct tm lt;
        localtime_r(&next_nightly_reboot, &lt);
        ESP_LOGI(TAG, "Nightly reboot scheduled for %04d-%02d-%02d %02d:%02d local",
                 lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    } else if (now_wall >= next_nightly_reboot) {
        ESP_LOGW(TAG, "Nightly maintenance reboot (%02d:00 local)", NIGHTLY_REBOOT_HOUR);
        restart_device();
    }

    /* Night dimming - see s_tap_layer / NIGHT_DIM_OPA. A window that ends
     * before it starts wraps past midnight; one that ends where it starts is
     * empty. */
    struct tm now_lt;
    localtime_r(&now_wall, &now_lt);
    const int now_min = now_lt.tm_hour * 60 + now_lt.tm_min;
    const int from = g_cfg->dim_start, to = g_cfg->dim_end;
    bool want_dim = g_cfg->dim_enabled &&
                    (from <= to ? (now_min >= from && now_min < to) : (now_min >= from || now_min < to));
    if (want_dim != s_night_dim) {
        s_night_dim = want_dim;
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            night_apply();
            esp_lv_adapter_unlock();
        }
        ESP_LOGI(TAG, "Night %s %s (local time %02d:%02d)", g_cfg->night_off ? "screen off" : "dimming",
                 want_dim ? "on" : "off", now_lt.tm_hour, now_lt.tm_min);
    }
}

/* A full pass, so WiFi, the fetching and the screen all work: keep this
 * firmware. Until then a freshly updated one is on probation, and a restart
 * goes back to the previous (see wifi_provision's h_ota). No-op otherwise. */
static void keep_firmware(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New firmware works - keeping it");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

static void yr_weather_task(void *arg)
{
    /* Connects in station mode, or blocks forever in the setup portal (and
     * reboots when the form is saved). */
    /* The clock (clock.c) starts in the background as WiFi connects; until
     * it's set, the loop below treats the time of day as unknown. */
    wifi_provision_before_connect(clock_start);
    wifi_provision_connect(g_cfg, provision_status_cb);
    wifi_provision_add_get_handler("/screen.png", screenshot_handler);
    wifi_provision_add_get_handler("/screen", screen_handler);
    diag_start();
    updater_start(xTaskGetCurrentTaskHandle());

    /* Let WiFi's own connection-setup buffers settle before hitting it with
     * a large TLS handshake - the two compete hard for the same scarce
     * internal DRAM in the first moment after association. */
    vTaskDelay(pdMS_TO_TICKS(3000));


    int active_view = -1; /* -1 forces a first render; else == g_view_index */
    int saved_view = app_config_load_last_view();
    TickType_t view_since = 0; /* when active_view was switched to */
    bool refetch_sel = false;

    while (1) {
        wd_weather_beat();
        nightly_housekeeping();

        /* Adopt a view switch made by a tap or the rotation (view_enter has
         * already put the screen up). */
        const int want_view = g_view_index;
        const view_stop_t *stop = &s_stops[want_view];
        if (want_view != active_view) {
            const bool first = (active_view < 0);
            active_view = want_view;
            view_since = xTaskGetTickCount();
            /* A tap to a weather screen refetches its forecast even if the
             * cache isn't stale yet; the rotation, passing by every few
             * seconds, shows it as kept fresh by the 10-minute refresh. */
            refetch_sel = (stop->kind == STOP_WEATHER) && !s_view_auto;

            /* Each client keeps its HTTPS connection open between polls; drop
             * those (and their TLS buffers) not needed on this screen. */
            if (stop->kind != STOP_RADAR) {
                adsb_client_close();
            }
            if (stop->kind != STOP_SHIPS) {
                ais_client_close();
            }
            if (stop->kind != STOP_RAIN) {
                rain_client_close();
            }
            if (stop->kind != STOP_DEPARTURES) {
                entur_client_close();
            }

            if (first) {
                /* Put the screen's own status back after the WiFi connect's. */
                if (esp_lv_adapter_lock(-1) == ESP_OK) {
                    if (g_view_index == want_view) {
                        view_enter(want_view);
                    }
                    esp_lv_adapter_unlock();
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(VIEW_SETTLE_MS));
                if (g_view_index != active_view) {
                    continue;
                }
            }
        }

        uint32_t wait_ms;
        switch ((stop_kind_t)stop->kind) {
        case STOP_RADAR:
        case STOP_SHIPS:
        case STOP_RAIN:
            wait_ms = radar_poll(stop->kind, stop->loc, active_view);
            break;
        case STOP_DEPARTURES:
            wait_ms = departures_poll(stop->loc, active_view);
            break;
        case STOP_AIR:
            wait_ms = air_poll(stop->loc, active_view);
            break;
        case STOP_WEEK:
            wait_ms = week_poll(stop->loc, active_view);
            break;
        case STOP_TIDE:
            wait_ms = tide_poll(stop->loc, active_view);
            break;
        case STOP_CALENDAR:
            wait_ms = calendar_poll(active_view);
            break;
        default:
            wait_ms = weather_poll(stop->kind == STOP_OVERVIEW, stop->loc, refetch_sel, active_view);
            refetch_sel = false;
            break;
        }
        if (g_view_index != active_view) {
            continue;
        }
        wd_weather_beat();
        keep_firmware();

        /* Firmware updates: a check or install that is due or was asked for
         * on the setup page. The current screen puts its status back after. */
        if (updater_poll(&wait_ms) && esp_lv_adapter_lock(-1) == ESP_OK) {
            if (g_view_index == active_view) {
                view_enter(active_view);
            }
            esp_lv_adapter_unlock();
        }

        /* So a reboot of any kind - nightly, power cycle, crash - comes back
         * showing the screen last picked by tap instead of the overview. */
        const TickType_t shown_for = xTaskGetTickCount() - view_since;
        if (!s_view_auto && active_view != saved_view) {
            if (shown_for >= pdMS_TO_TICKS(LAST_VIEW_SAVE_MS)) {
                app_config_save_last_view((uint8_t)active_view);
                saved_view = active_view;
            } else if (wait_ms > LAST_VIEW_SAVE_MS) {
                wait_ms = LAST_VIEW_SAVE_MS; /* come back to save it */
            }
        }

        /* A tap or the rotation notifies us, cutting the wait short. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

/* Every failed allocation is logged, with who asked: running out of PSRAM
 * or internal DRAM otherwise shows up only as some unrelated failure. */
static void alloc_failed_cb(size_t size, uint32_t caps, const char *function_name)
{
    esp_rom_printf("ALLOC FAILED: %u bytes, caps 0x%x, in %s; free psram %u (largest %u), int %u (largest %u)\n",
                   (unsigned)size, (unsigned)caps, function_name ? function_name : "?",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* The panel's bounce buffers are refilled from PSRAM in its DMA interrupt,
 * which is installed on the core that creates the panel. app_main runs on
 * core 0 beside WiFi, whose interrupts and critical sections delayed the
 * refill until it fell a whole buffer behind and the bottom lines showed at
 * the top. So the panel (and touch) are created from a short task on
 * core 1. */
typedef struct {
    uint8_t fb_count;
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;
    TaskHandle_t caller;
} lcd_init_args_t;

static void lcd_init_task(void *arg)
{
    lcd_init_args_t *a = arg;
    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init(a->fb_count, &a->panel, &a->touch));
    xTaskNotifyGive(a->caller);
    vTaskDelete(NULL);
}

void app_main(void)
{
    diag_init();
    heap_caps_register_failed_alloc_callback(alloc_failed_cb);
    /* Logged first, unconditionally, so a boot that never reaches "Got IP"
     * still leaves a trail: reason 1 is a normal power-on, but e.g. 3 (panic)
     * or 8 (task/int watchdog) points at a crash-reboot loop rather than a
     * genuinely stuck WiFi connect. */
    ESP_LOGI(TAG, "Reset reason: %d", esp_reset_reason());

    /* Europe/Oslo: CET (UTC+1), CEST (UTC+2) from the last Sunday of March
     * 02:00 to the last Sunday of October 03:00. Process-wide, so yr_client's
     * localtime_r() calls render the forecast's UTC timestamps in Norwegian
     * wall-clock time. Set before the weather task starts. */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    g_cfg = heap_caps_calloc(1, sizeof(*g_cfg), MALLOC_CAP_SPIRAM);
    assert(g_cfg != NULL);
    app_config_load(g_cfg);
    yr_client_set_contact_email(g_cfg->yr_email);

    /* Resume on whatever screen was showing before this boot (see
     * app_config_save_last_view) rather than always starting at the
     * overview. Clamped in case the location count shrank since. */
    build_stops();
    weather_init();
    g_view_index = app_config_load_last_view();
    if (g_view_index >= s_stop_count) {
        g_view_index = 0;
    }

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    lcd_init_args_t lcd = { .fb_count = frame_buffer_count, .caller = xTaskGetCurrentTaskHandle() };
    xTaskCreatePinnedToCore(lcd_init_task, "lcd_init", 4096, &lcd, tskIDLE_PRIORITY + 5, NULL, 1);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_lcd_panel_handle_t panel_handle = lcd.panel;
    esp_lcd_touch_handle_t touch_handle = lcd.touch;
    ESP_ERROR_CHECK(waveshare_rgb_lcd_backlight_on());

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        panel_handle,
        NULL,
        EXAMPLE_LCD_H_RES,
        EXAMPLE_LCD_V_RES,
        rotation);
    disp_config.profile.use_psram = true;
    /* DEFAULT_RGB (TRIPLE_PARTIAL) allocates a hor_res * buffer_height partial
     * draw buffer, and the adapter hardcodes it to *internal* DRAM regardless
     * of use_psram. At the default 50 lines that's ~80 KB of the ~230 KB
     * internal heap - enough that esp_wifi_init() and FreeType glyph
     * rendering start failing their allocations. 16 lines (~25 KB) leaves the
     * headroom; the cost is more (smaller) flush cycles, which is fine for a
     * near-static weather screen. Trimmed further (16 -> 10) to make room for
     * the overview screen and the wind markers - below ~10 the WiFi PHY's own
     * esp_timer_create() starts failing its allocation at startup. */
    disp_config.profile.buffer_height = 10;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != NULL);

    if (touch_handle != NULL) {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != NULL);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    ESP_LOGI(TAG, "Starting LVGL YR 48h forecast");
    init_fonts();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        build_ui(lv_screen_active());
        esp_lv_adapter_unlock();
    }
    wd_start();

    /* This task must keep its stack in internal RAM. With the app running
     * from PSRAM, plain flash reads and writes (NVS) leave the cache on, but
     * memory-mapping flash still freezes it - and the OTA functions map the
     * otadata partition (keep_firmware(), updates), which asserts that the
     * calling task's stack isn't in PSRAM (crash-looped when tried). */
    xTaskCreate(yr_weather_task, "yr_weather", YR_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, &s_yr_task);
}
