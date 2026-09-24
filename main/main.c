#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"
#include "esp_netif_sntp.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmap_generate_fonts.h"
#include "nvs_flash.h"
#include "waveshare_rgb_lcd_port.h"
#include "adsb_client.h"
#include "ais_client.h"
#include "app_config.h"
#include "met_alerts_client.h"
#include "wifi_provision.h"
#include "yr_client.h"

static const char *TAG = "lvgl9_demo";

/* The hourly Locationforecast is updated seldom upstream - poll it slowly.
 * The Nowcast (radar precipitation for the next ~2 h) refreshes every 5 min,
 * so poll it on its own faster cadence and splice it in ahead of the hourly
 * points. */
#define WEATHER_REFRESH_INTERVAL_MS (10 * 60 * 1000)
#define NOWCAST_REFRESH_INTERVAL_MS (5 * 60 * 1000)
#define WEATHER_RETRY_INTERVAL_MS (20 * 1000)
/* MET Norway's severe weather alerts ("farevarsel") change far less often
 * than the forecast - polled on its own, slower, independent cadence so one
 * data source's staleness never forces a refetch of the other. */
#define ALERT_REFRESH_INTERVAL_MS (10 * 60 * 1000)
/* Take every Nth nowcast step (5 min apart) into the merged series: every
 * 2nd = 10-minute resolution for the near term, still 6x finer than hourly
 * without over-compressing the rest of the chart. */
#define NOWCAST_MERGE_STRIDE 2
#define NUM_HOUR_LABELS 8
#define YR_TASK_STACK_SIZE 8192

/* Restart once a day, at this local hour, purely as memory-pressure
 * housekeeping (a fresh boot resets any accumulated heap fragmentation).
 * Needs the wall clock to actually be synced (see the SNTP setup below) -
 * without it "now" would read as the 1970 epoch, so scheduling is skipped
 * until a sync has happened. The current screen survives the restart (see
 * app_config_save_last_view). */
#define NIGHTLY_REBOOT_HOUR 2
/* Treat the clock as synced once it reads past this (2023-01-01 UTC) -
 * comfortably below "now" for the life of this project, comfortably above
 * the unsynced epoch. */
#define PLAUSIBLE_EPOCH_S 1672531200

/* Night dimming: the backlight itself can't be dimmed (see s_tap_layer), so
 * a translucent black layer over the whole screen stands in for it during
 * these local hours. LV_OPA_70 cuts the effective brightness a lot while
 * keeping high-contrast text/lines legible in a dark room. */
#define NIGHT_DIM_START_HOUR 22
#define NIGHT_DIM_END_HOUR   7
#define NIGHT_DIM_OPA         LV_OPA_70

#define ICON_ROW_Y 44
#define ICON_SIZE 48

#define CHART_X 20
#define CHART_W 760
/* Main temperature/precipitation chart. */
#define CHART_Y 100
#define CHART_H 244
/* Precipitation bars are drawn on a taller-than-needed axis so they only
 * occupy the bottom fraction of the shared chart, leaving the rest of the
 * height for the temperature line to read clearly. */
#define PRECIP_AXIS_COMPRESSION 3
#define TEMP_LINE_WIDTH 3

/* Wind section stacked below the main chart: a row of direction arrows over a
 * short wind-speed bar chart, sharing the main chart's x-scale. */
#define WIND_DIR_ROW_Y (CHART_Y + CHART_H + 4)
#define WIND_ARROW_SIZE 28
#define WIND_CHART_Y (WIND_DIR_ROW_Y + WIND_ARROW_SIZE + 2)
#define WIND_CHART_H 54

#define HOUR_ROW_Y (WIND_CHART_Y + WIND_CHART_H + 6)

/* Chart value markers. Temperature: the global high and low are always
 * labelled; precipitation/wind: the global peak is always labelled. Further
 * local extrema get a label only once at least MARKER_MIN_GAP_H hours have
 * passed since the previously shown marker OF THE SAME TYPE (a max only
 * spaces against the previous max, a min against the previous min). So a fast
 * swing can still show a peak and the trough right after it, while a long,
 * gently varying forecast stays uncrowded - which also keeps the label pools
 * (and their scarce internal-DRAM widgets) small. */
#define MARKER_MIN_GAP_H 8
/* A non-global local temperature extremum earns a label only if it stands at
 * least this far (deg C) clear of its surroundings - keeps a shallow wiggle
 * next to a real peak or trough from getting its own number. */
#define MARKER_TEMP_MIN_SWING 1.0f
/* Pools sized for the realistic worst case under the 8 h same-type spacing
 * over a ~48 h forecast (a handful of maxima + minima for temperature; fewer
 * peaks for the smoother precipitation and wind series). An overflow just
 * drops the least important trailing label - no crash. */
#define TEMP_MARKER_POOL 8
#define PRECIP_MARKER_POOL 2 /* the two highest max-precipitation peaks - see place_precip_markers */
#define WIND_MARKER_POOL 2   /* the two highest gust peaks - see place_wind_markers */
/* Wind/gust and precipitation-range peak labels (pick_top_peaks) use their
 * own, tighter spacing than MARKER_MIN_GAP_H: a second peak within this many
 * hours of a stronger one is the same event, not a distinct one, so only the
 * stronger of the two is labelled. */
#define PEAK_LABEL_MIN_GAP_H 6

/* Overview screen: a table with one row per location and OV_COLS time columns
 * OV_STEP_H hours apart. Each cell shows the weather icon, the temperature at
 * that hour and the precipitation summed over the following OV_STEP_H hours.
 * It is always the first stop when cycling (see build_stops). */
#define OV_COLS     4
#define OV_STEP_H   6
#define OV_X        10
#define OV_NAME_W   150            /* wide enough for the alert dot + the longest
                                    * location names (e.g. "Kvaløysletta") on one line */
#define OV_COL_W    157            /* (800 - OV_X*2 - OV_NAME_W) / OV_COLS   */
#define OV_TITLE_Y  6
#define OV_HDR_Y    42
#define OV_BODY_Y   64
/* 76, not 80: at the 5-location max that leaves a clear strip at the very
 * bottom of the screen for the IP-address label. */
#define OV_ROW_H    76
#define OV_ICON     34
#define OV_ALERT_DOT 18 /* the per-row severe-weather-alert badge, see s_ov_alert */

/* Aircraft radar screen (one per location that has it ticked in the setup
 * portal): a sonar-style plot on the left, a table of the nearest aircraft on
 * the right. Everything is drawn in one custom draw callback rather than as
 * LVGL objects - an object per aircraft/label would cost scarce internal DRAM. */
#define RADAR_CX            250
#define RADAR_CY            262
#define RADAR_R             190   /* outer ring radius, px */
#define RADAR_LIST_X        500
#define RADAR_LIST_Y        78
#define RADAR_LIST_ROW_H    26
#define RADAR_LIST_ROWS     14
#define RADAR_TAGS          10    /* aircraft that also get a callsign tag on the plot */
#define KM_PER_NM           1.852f
/* One request per poll; adsb.fi allows at most 1/s. */
#define ADSB_POLL_MS        5000
/* Aircraft are dead-reckoned between polls, so repaint now and then. */
#define RADAR_REDRAW_MS     2000
/* Ship traffic (same screen as the aircraft radar): one BarentsWatch request
 * per 30 s. Ships are dead-reckoned in between like the aircraft, but never
 * further than SHIP_EXTRAP_MAX_S past their last report. */
#define SHIP_POLL_MS        30000
#define SHIP_EXTRAP_MAX_S   600.0f
#define SHIP_VEC_MIN        10    /* course vector: where it will be in this many minutes */
#define SHIP_TAG_MAX_W      120   /* px; longer names are shortened on the plot */

static const lv_font_t *s_font_body;
static const lv_font_t *s_font_large;

/* Runtime settings (WiFi + forecast locations), from NVS via the setup portal
 * or the compiled-in defaults. Loaded once in app_main. */
static app_config_t s_cfg;

/* The screens a tap cycles through, in order (see build_stops): the overview
 * table (always present, even with zero or one weather location - it's the
 * only place the device's IP address is shown, needed to reach the setup
 * portal for further configuration), then for each location whichever of its
 * weather screen, aircraft radar and ship traffic are enabled, in that order. */
typedef enum { STOP_OVERVIEW = 0, STOP_WEATHER = 1, STOP_RADAR = 2, STOP_SHIPS = 3 } stop_kind_t;
typedef struct {
    uint8_t kind; /* stop_kind_t */
    uint8_t loc;  /* location index; unused for the overview */
} view_stop_t;
static view_stop_t s_stops[1 + 3 * APP_CONFIG_MAX_LOCATIONS];
static int s_stop_count;

/* Locations that show weather, in order: the rows of the overview and the
 * "n/m" counter on the weather screens. A location can be radar-only. */
static int s_weather_count;
static uint8_t s_wx_loc[APP_CONFIG_MAX_LOCATIONS]; /* weather index -> location */
static int8_t s_wx_pos[APP_CONFIG_MAX_LOCATIONS];  /* location -> weather index, -1 if none */
static bool s_any_radar; /* some location shows aircraft or ships (they share the radar screen) */

/* Index into s_stops of the screen on show. Advanced by a tap; the weather
 * task watches it and re-renders. */
static volatile int s_view_index;
static TaskHandle_t s_yr_task;

static lv_obj_t *s_status_label;
static lv_obj_t *s_tap_layer;     /* full-screen tap catcher; also the night-dim overlay */
/* Aircraft radar / ship traffic colours, one set per theme. ship[] is per
 * ais_category_t. */
typedef struct {
    uint32_t bg, disc, ring, txt, dim, plane, vec, apt, coast, water;
    uint32_t ship[AIS_CAT_COUNT];
} radar_palette_t;

static const radar_palette_t RADAR_DARK = {
    .bg = 0x050B12, .disc = 0x0A1E30, .ring = 0x1F6E45, .txt = 0xDDE6EE,
    .dim = 0x8AA0B4, .plane = 0xFF5A4F, .vec = 0xE060E0, .apt = 0x3FBFB0,
    .coast = 0x6F93AD, .water = 0x0F3A5F,
    .ship = {
        [AIS_CAT_OTHER] = 0xB0BEC5, [AIS_CAT_CARGO] = 0x66BB6A, [AIS_CAT_TANKER] = 0xFF7043,
        [AIS_CAT_PASSENGER] = 0x42A5F5, [AIS_CAT_FISHING] = 0xFFCA28, [AIS_CAT_LEISURE] = 0xE040FB,
        [AIS_CAT_TUG] = 0x26C6DA,
    },
};
static const radar_palette_t RADAR_LIGHT = {
    .bg = 0xEEF2F6, .disc = 0xFFFFFF, .ring = 0x6BAF8A, .txt = 0x1B2631,
    .dim = 0x5D6D7E, .plane = 0xD62D20, .vec = 0xA83CA8, .apt = 0x1B8A7E,
    .coast = 0x7F9AB0, .water = 0xD4E8F7,
    .ship = {
        [AIS_CAT_OTHER] = 0x607D8B, [AIS_CAT_CARGO] = 0x2E7D32, [AIS_CAT_TANKER] = 0xD84315,
        [AIS_CAT_PASSENGER] = 0x1565C0, [AIS_CAT_FISHING] = 0xB28704, [AIS_CAT_LEISURE] = 0x9C27B0,
        [AIS_CAT_TUG] = 0x00838F,
    },
};
static const radar_palette_t *s_rp = &RADAR_DARK; /* set from the theme in build_radar */
static lv_obj_t *s_detail_root;   /* holds every per-location detail widget  */
static lv_obj_t *s_overview_root; /* holds the all-locations overview table   */
static lv_obj_t *s_location_label;
static lv_obj_t *s_updated_label;
static lv_obj_t *s_alert_label; /* top-centre: the selected location's worst active alert, if any */

/* Aircraft radar, also used for ship traffic (built only if some location has
 * either enabled). s_ship_mode picks which the screen is showing. */
static lv_obj_t *s_radar_root;
static lv_obj_t *s_radar_title;
static lv_obj_t *s_radar_info;
static lv_obj_t *s_radar_canvas;
static adsb_result_t *s_radar_data; /* PSRAM; last fetch for the location on show */
static bool s_radar_valid;
static uint32_t s_radar_tick;       /* lv_tick_get() when s_radar_data / s_ship_data was stored */
static ais_result_t *s_ship_data;   /* PSRAM; last ship fetch for the location on show */
static bool s_ship_mode;

/* Coastline under the aircraft or ships (see coast_render): an A8 coverage
 * image of the radar disc, drawn in s_rp->coast, over an A8 mask of the
 * water, drawn in s_rp->water. They show s_coast_loc at
 * s_coast_km; s_coast_valid gates drawing them. */
#define COAST_D  (2 * RADAR_R + 1)
static uint8_t *s_coast_px;         /* PSRAM, COAST_D x COAST_D */
static uint8_t *s_water_px;         /* PSRAM, COAST_D x COAST_D */
static lv_image_dsc_t s_coast_img;
static lv_image_dsc_t s_water_img;
/* Coastline segment middles with the normal to their water side, noted by
 * coast_seed for coast_fill_water. */
typedef struct {
    float x, y, nx, ny;
} water_seed_t;
#define WATER_SEEDS_MAX 32768
static water_seed_t *s_water_seeds; /* PSRAM, WATER_SEEDS_MAX */
static int s_water_n_seeds;
static bool s_coast_valid;
static int s_coast_loc = -1;
static int s_coast_km;
static int s_radar_loc = -1;        /* location the radar screen is set to */

/* The radar screen now shows `loc` at `range_km` (adapter lock held): stop
 * drawing a coastline image made for anything else until coast_render redoes it. */
static void coast_mark(int loc, int range_km)
{
    s_radar_loc = loc;
    if (loc != s_coast_loc || range_km != s_coast_km) {
        s_coast_valid = false;
    }
}

/* Overview table widgets (built only when >= 2 locations). */
static lv_obj_t *s_ov_title;
static lv_obj_t *s_ov_ip_label;   /* bottom-right: the address to browse to for setup */
static lv_obj_t *s_ov_heap_label; /* bottom-left: free internal-DRAM bytes */
static lv_obj_t *s_ov_hdr[OV_COLS];
static lv_obj_t *s_ov_name[APP_CONFIG_MAX_LOCATIONS];
static lv_obj_t *s_ov_icon[APP_CONFIG_MAX_LOCATIONS][OV_COLS];
static lv_obj_t *s_ov_cell[APP_CONFIG_MAX_LOCATIONS][OV_COLS];
static lv_obj_t *s_ov_alert[APP_CONFIG_MAX_LOCATIONS]; /* small badge beside the name, worst active alert's colour */

/* Per-location hourly forecast cache (PSRAM), kept warm for every location so
 * the overview can show them all at once. s_fc_cache[i] is allocated in the
 * weather task; s_fc_valid[i] gates reads; s_fc_tk[i] is its last refresh. */
static yr_forecast_t *s_fc_cache[APP_CONFIG_MAX_LOCATIONS];
static bool s_fc_valid[APP_CONFIG_MAX_LOCATIONS];
static TickType_t s_fc_tk[APP_CONFIG_MAX_LOCATIONS];

/* Per-location severe weather alerts (PSRAM), same shape as the forecast
 * cache above and refreshed on its own cadence (see ALERT_REFRESH_INTERVAL_MS). */
static met_alerts_t *s_alert_cache[APP_CONFIG_MAX_LOCATIONS];
static bool s_alert_valid[APP_CONFIG_MAX_LOCATIONS];
static TickType_t s_alert_tk[APP_CONFIG_MAX_LOCATIONS];

/* s_precip_max_chart is the frame (background/border/gridlines) for the
 * whole precip+temp area, and sits behind s_precip_chart (created first, so
 * it's drawn first / lower z-order) and s_temp_line - both fully transparent
 * overlays, so this chart's own (taller, paler) max-precipitation bars show
 * through above wherever the shorter min bar doesn't reach. */
static lv_obj_t *s_precip_max_chart;
static lv_chart_series_t *s_precip_max_series;
static lv_obj_t *s_precip_chart;
static lv_chart_series_t *s_precip_series;
static lv_obj_t *s_temp_line;
static lv_obj_t *s_temp_markers[TEMP_MARKER_POOL];
static lv_obj_t *s_precip_markers[PRECIP_MARKER_POOL];

/* s_gust_chart is a plain, frameless bars-only layer sitting behind
 * s_wind_chart (created first, so it's drawn first / lower z-order);
 * s_wind_chart's own background is made transparent so the taller gust bars
 * show through above wherever the shorter wind bar doesn't reach - the same
 * "frame widget + transparent overlay" trick as s_precip_max_chart above. */
static lv_obj_t *s_gust_chart;
static lv_chart_series_t *s_gust_series;
static lv_obj_t *s_wind_chart;
static lv_chart_series_t *s_wind_series;
static lv_obj_t *s_wind_markers[WIND_MARKER_POOL];
static lv_obj_t *s_wind_dir_arrows[NUM_HOUR_LABELS];

static lv_obj_t *s_hour_labels[NUM_HOUR_LABELS];
static lv_obj_t *s_icon_slots[NUM_HOUR_LABELS];

static int32_t s_precip_chart_data[YR_FORECAST_MAX_POINTS];     /* millimeters * 10 */
static int32_t s_precip_max_chart_data[YR_FORECAST_MAX_POINTS]; /* millimeters * 10 */
static int32_t s_wind_chart_data[YR_FORECAST_MAX_POINTS];   /* m/s * 10 */
static int32_t s_gust_chart_data[YR_FORECAST_MAX_POINTS];   /* m/s * 10 */
static lv_point_precise_t s_temp_line_points[YR_FORECAST_MAX_POINTS];
/* The temperature each s_temp_line_points entry was plotted from, and the
 * chart y of 0 degrees C, so temp_line_draw_cb can colour the sub-zero parts
 * of the line separately. */
static float s_temp_line_values[YR_FORECAST_MAX_POINTS];
static uint32_t s_temp_line_count;
static lv_color_t s_temp_warm_color;
static lv_color_t s_temp_cold_color;

static int32_t round_to_int(float v)
{
    return (int32_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

/* Point the icon widget at the MET Norway PNG for this symbol_code (packed
 * into the asset drive as "F:<symbol_code>.png"). An empty code just hides
 * the widget. The file set is the full github.com/metno/weathericons list,
 * so every code the API returns resolves directly. */
static void set_weather_icon(lv_obj_t *img, const char *symbol_code)
{
    if (symbol_code == NULL || symbol_code[0] == '\0') {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char path[80];
    snprintf(path, sizeof(path), "F:%s.png", symbol_code);
    lv_image_set_src(img, path);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
}

static void init_fonts(void)
{
    /* Mount the "fonts" SPIFFS partition (built by spiffs_create_partition_assets
     * in main/CMakeLists.txt) as the "F:" drive: LVGL's FreeType binding opens
     * the .ttf by path, and the MET weather icons are loaded the same way
     * (F:<symbol_code>.png, decoded by esp_lv_decoder). */
    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "fonts",
        .max_files = MMAP_FONTS_FILES,
        .checksum = MMAP_FONTS_CHECKSUM,
        .flags = { .mmap_enable = 1 },
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

    const char *font_path = "F:MontserratMedium.ttf";

    esp_lv_adapter_ft_font_handle_t body_handle = NULL;
    const esp_lv_adapter_ft_font_config_t body_cfg = ESP_LV_ADAPTER_FT_FONT_FILE_CONFIG(
        font_path, 17, ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL);
    ESP_ERROR_CHECK(esp_lv_adapter_ft_font_init(&body_cfg, &body_handle));
    s_font_body = esp_lv_adapter_ft_font_get(body_handle);
    assert(s_font_body != NULL);

    esp_lv_adapter_ft_font_handle_t large_handle = NULL;
    const esp_lv_adapter_ft_font_config_t large_cfg = ESP_LV_ADAPTER_FT_FONT_FILE_CONFIG(
        font_path, 25, ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL);
    ESP_ERROR_CHECK(esp_lv_adapter_ft_font_init(&large_cfg, &large_handle));
    s_font_large = esp_lv_adapter_ft_font_get(large_handle);
    assert(s_font_large != NULL);
}

/* Tap the right half of the screen: next stop (overview -> location 1 ->
 * location 2 -> ... -> overview); tap the left half: previous stop. Wakes the weather task so it
 * re-renders / refetches. Runs in the LVGL context (which already holds the
 * adapter lock), so it only pokes volatiles + a notify. */
static void screen_touch_cb(lv_event_t *e)
{
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

    int next = s_view_index + (left ? -1 : 1);
    if (next >= s_stop_count) {
        next = 0;
    } else if (next < 0) {
        next = s_stop_count - 1;
    }
    s_view_index = next;
    ESP_LOGI(TAG, "Tap: view %d/%d", next, s_stop_count);
    if (s_yr_task != NULL) {
        xTaskNotifyGive(s_yr_task);
    }
}

/* Lay out the cycle of screens from the configuration. */
static void build_stops(void)
{
    s_weather_count = 0;
    s_any_radar = false;
    for (int i = 0; i < s_cfg.location_count; i++) {
        s_wx_pos[i] = -1;
        if (s_cfg.show[i] & APP_SHOW_WEATHER) {
            s_wx_pos[i] = (int8_t)s_weather_count;
            s_wx_loc[s_weather_count++] = (uint8_t)i;
        }
        if (s_cfg.show[i] & (APP_SHOW_RADAR | APP_SHOW_SHIPS)) {
            s_any_radar = true;
        }
    }

    s_stop_count = 0;
    /* Always a stop, regardless of location_count: it's the only screen that
     * shows the device's IP address, so it must always be reachable by tap. */
    s_stops[s_stop_count++] = (view_stop_t){ STOP_OVERVIEW, 0 };
    for (int i = 0; i < s_cfg.location_count; i++) {
        if (s_cfg.show[i] & APP_SHOW_WEATHER) {
            s_stops[s_stop_count++] = (view_stop_t){ STOP_WEATHER, (uint8_t)i };
        }
        if (s_cfg.show[i] & APP_SHOW_RADAR) {
            s_stops[s_stop_count++] = (view_stop_t){ STOP_RADAR, (uint8_t)i };
        }
        if (s_cfg.show[i] & APP_SHOW_SHIPS) {
            s_stops[s_stop_count++] = (view_stop_t){ STOP_SHIPS, (uint8_t)i };
        }
    }
}

/* Show the overview table, the per-location weather screen or the radar. The
 * status label and tap layer sit above all of them and are left alone. */
static void show_view(stop_kind_t kind)
{
    lv_obj_t *roots[3];
    roots[STOP_OVERVIEW] = s_overview_root;
    roots[STOP_WEATHER] = s_detail_root;
    roots[STOP_RADAR] = s_radar_root;
    int shown = (kind == STOP_SHIPS) ? STOP_RADAR : (int)kind;
    for (int k = 0; k < 3; k++) {
        if (roots[k] == NULL) {
            continue;
        }
        if (k == shown) {
            lv_obj_clear_flag(roots[k], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(roots[k], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* The status text sits above whichever screen is shown. On the radar it
     * takes the radar's text colour, and is moved over the table half so it
     * doesn't sit on the plot. */
    if (kind == STOP_RADAR || kind == STOP_SHIPS) {
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(s_rp->txt), 0);
        lv_obj_align(s_status_label, LV_ALIGN_CENTER, 245, 0);
    } else {
        lv_obj_remove_local_style_prop(s_status_label, LV_STYLE_TEXT_COLOR, 0);
        lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);
    }
}

/* Build the overview table into `root`: a title, a header row of clock hours
 * (filled in each refresh), then one row per location with a name cell and
 * OV_COLS cells of {weather icon, temperature, precipitation}. Always called;
 * the row loop below is simply empty when no location shows weather - the IP
 * and free-heap footnotes are the only content in that case. */
static void build_overview(lv_obj_t *root)
{
    const lv_color_t footnote = (s_cfg.theme == APP_THEME_DARK)
        ? lv_palette_main(LV_PALETTE_GREY) : lv_palette_darken(LV_PALETTE_GREY, 2);
    s_ov_title = lv_label_create(root);
    lv_obj_set_style_text_font(s_ov_title, s_font_large, 0);
    lv_obj_set_pos(s_ov_title, OV_X, OV_TITLE_Y);
    lv_label_set_text(s_ov_title, "Oversikt");

    lv_obj_t *sted = lv_label_create(root);
    lv_obj_set_pos(sted, OV_X, OV_HDR_Y);
    lv_label_set_text(sted, "Sted");

    for (int c = 0; c < OV_COLS; c++) {
        s_ov_hdr[c] = lv_label_create(root);
        lv_obj_set_pos(s_ov_hdr[c], OV_X + OV_NAME_W + c * OV_COL_W, OV_HDR_Y);
        lv_obj_set_width(s_ov_hdr[c], OV_COL_W);
        lv_label_set_text(s_ov_hdr[c], "");
    }

    /* One row per location that shows weather (row r = s_wx_loc[r]). */
    for (int i = 0; i < s_weather_count; i++) {
        int row_y = OV_BODY_Y + i * OV_ROW_H;

        /* The name text is indented to leave room for the alert dot at the
         * left of the row (below) - sized and positioned first so the dot
         * can align itself to it. */
        s_ov_name[i] = lv_label_create(root);
        lv_obj_set_pos(s_ov_name[i], OV_X + OV_ALERT_DOT + 6, row_y + OV_ICON / 2 - 4);
        lv_obj_set_width(s_ov_name[i], OV_NAME_W - OV_ALERT_DOT - 10);
        /* DOTS mode only truncates (rather than wrapping to a second line
         * that bleeds into the row below) when the object has a fixed,
         * single-line height - auto height lets it grow instead. */
        lv_obj_set_height(s_ov_name[i], lv_font_get_line_height(lv_obj_get_style_text_font(s_ov_name[i], 0)));
        lv_obj_set_style_text_align(s_ov_name[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(s_ov_name[i], LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(s_ov_name[i], s_cfg.locations[s_wx_loc[i]].name);

        /* Worst active alert's colour, or hidden. Vertically centred on the
         * name text regardless of font metrics, via align-to. */
        s_ov_alert[i] = lv_obj_create(root);
        lv_obj_remove_style_all(s_ov_alert[i]);
        lv_obj_set_size(s_ov_alert[i], OV_ALERT_DOT, OV_ALERT_DOT);
        lv_obj_set_style_radius(s_ov_alert[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_ov_alert[i], LV_OPA_COVER, 0);
        lv_obj_align_to(s_ov_alert[i], s_ov_name[i], LV_ALIGN_OUT_LEFT_MID, -4, 0);
        lv_obj_add_flag(s_ov_alert[i], LV_OBJ_FLAG_HIDDEN);

        for (int c = 0; c < OV_COLS; c++) {
            int cell_x = OV_X + OV_NAME_W + c * OV_COL_W;

            s_ov_icon[i][c] = lv_image_create(root);
            lv_obj_set_pos(s_ov_icon[i][c], cell_x + (OV_COL_W - OV_ICON) / 2, row_y);
            lv_obj_set_size(s_ov_icon[i][c], OV_ICON, OV_ICON);
            lv_image_set_inner_align(s_ov_icon[i][c], LV_IMAGE_ALIGN_CENTER);
            lv_image_set_scale(s_ov_icon[i][c], 256 * OV_ICON / ICON_SIZE);
            lv_obj_add_flag(s_ov_icon[i][c], LV_OBJ_FLAG_HIDDEN);

            s_ov_cell[i][c] = lv_label_create(root);
            lv_obj_set_pos(s_ov_cell[i][c], cell_x, row_y + OV_ICON + 1);
            lv_obj_set_width(s_ov_cell[i][c], OV_COL_W);
            lv_label_set_text(s_ov_cell[i][c], "");
        }
    }

    /* The address to browse to for WiFi/location setup (see wifi_provision).
     * Text is filled in each refresh by update_overview(); muted so it reads
     * as a footnote, not another data row. */
    s_ov_ip_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_ov_ip_label, footnote, 0);
    lv_obj_align(s_ov_ip_label, LV_ALIGN_BOTTOM_RIGHT, -OV_X, -4);
    lv_label_set_text(s_ov_ip_label, "");

    /* Free internal-DRAM bytes, the figure this project's memory work has
     * been tracking throughout - a running diagnostic, not user-facing data,
     * so it's a footnote like the IP label. */
    s_ov_heap_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_ov_heap_label, footnote, 0);
    lv_obj_align(s_ov_heap_label, LV_ALIGN_BOTTOM_LEFT, OV_X, -4);
    lv_label_set_text(s_ov_heap_label, "");
}

/* --------------------------------------------------------------------------
 * Aircraft radar
 *
 * The plot and table are painted straight into the draw layer from one
 * LV_EVENT_DRAW_MAIN handler. LVGL renders in horizontal strips (the draw
 * buffer is only a few lines tall), calling the handler once per strip, so
 * every primitive is culled against the strip first: creating a draw task per
 * element per strip would otherwise cost a lot of transient internal DRAM.
 * ------------------------------------------------------------------------ */

static bool radar_area_hits_clip(const lv_layer_t *layer, const lv_area_t *a)
{
    const lv_area_t *c = &layer->_clip_area;
    return a->x1 <= c->x2 && a->x2 >= c->x1 && a->y1 <= c->y2 && a->y2 >= c->y1;
}

static bool radar_vis(const lv_layer_t *layer, int x1, int y1, int x2, int y2)
{
    lv_area_t a = { x1, y1, x2, y2 };
    return radar_area_hits_clip(layer, &a);
}

static void radar_text(lv_layer_t *layer, const char *txt, int x, int y, int w,
                       lv_text_align_t align, lv_color_t color)
{
    int h = lv_font_get_line_height(s_font_body);
    if (!radar_vis(layer, x, y, x + w - 1, y + h - 1)) {
        return;
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = s_font_body;
    d.color = color;
    d.text = txt;
    d.text_local = 1; /* drawing is deferred; LVGL copies the string */
    d.align = align;
    lv_area_t a = { x, y, x + w - 1, y + h - 1 };
    lv_draw_label(layer, &d, &a);
}

static void radar_line(lv_layer_t *layer, int x1, int y1, int x2, int y2, int width, lv_color_t color)
{
    int lo_x = x1 < x2 ? x1 : x2, hi_x = x1 < x2 ? x2 : x1;
    int lo_y = y1 < y2 ? y1 : y2, hi_y = y1 < y2 ? y2 : y1;
    if (!radar_vis(layer, lo_x - width, lo_y - width, hi_x + width, hi_y + width)) {
        return;
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = width;
    d.color = color;
    d.opa = LV_OPA_COVER;
    lv_draw_line(layer, &d);
}

/* A circle centred on the radar: an outline, optionally filled. */
static void radar_circle(lv_layer_t *layer, int r, int border_w, lv_color_t border,
                         bool fill, lv_color_t fill_color)
{
    lv_area_t a = { RADAR_CX - r, RADAR_CY - r, RADAR_CX + r, RADAR_CY + r };
    if (!radar_area_hits_clip(layer, &a)) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
    d.bg_color = fill_color;
    d.border_width = border_w;
    d.border_color = border;
    d.border_opa = border_w > 0 ? LV_OPA_COVER : LV_OPA_TRANSP;
    lv_draw_rect(layer, &d, &a);
}

static void radar_triangle(lv_layer_t *layer, const int pts[3][2], lv_color_t color)
{
    int lo_x = pts[0][0], hi_x = pts[0][0], lo_y = pts[0][1], hi_y = pts[0][1];
    for (int i = 1; i < 3; i++) {
        if (pts[i][0] < lo_x) lo_x = pts[i][0];
        if (pts[i][0] > hi_x) hi_x = pts[i][0];
        if (pts[i][1] < lo_y) lo_y = pts[i][1];
        if (pts[i][1] > hi_y) hi_y = pts[i][1];
    }
    if (!radar_vis(layer, lo_x, lo_y, hi_x, hi_y)) {
        return;
    }
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    for (int i = 0; i < 3; i++) {
        d.p[i].x = pts[i][0];
        d.p[i].y = pts[i][1];
    }
    d.color = color;
    d.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &d);
}

/* Altitude in metres (to the nearest 10), or kilometres from 1000 m up. The
 * feed reports feet. */
static void radar_fmt_alt(char *buf, size_t n, int32_t alt_ft)
{
    if (alt_ft == ADSB_ALT_UNKNOWN) {
        snprintf(buf, n, "-");
        return;
    }
    float m = (float)alt_ft * 0.3048f;
    if (m < 995.0f) {
        snprintf(buf, n, "%d m", (int)lroundf(m / 10.0f) * 10);
    } else {
        snprintf(buf, n, "%.1f km", (double)(m / 1000.0f));
    }
}


/* Airports inside the radar's range, drawn under the aircraft: runway lines
 * (or a dot when they'd be too small to see) and an ICAO label. The full table
 * (components/airports.bin, built by scripts/build_airports.py from OurAirports)
 * lives in flash; only the handful in range are kept, precomputed as km offsets
 * from the radar centre whenever the radar is shown for a location. */
#define RADAR_APT_MAX      24
#define RADAR_APT_RWY_MAX  4    /* runways kept per airport */

typedef struct {
    char label[5];     /* IATA code, or the ICAO code where the airport has none */
    float x_km, y_km;  /* east / north of the radar centre */
    uint8_t n_rwy;
    uint8_t first_rwy; /* index into s_radar_rwy */
} radar_apt_t;

typedef struct {
    float x1, y1, x2, y2; /* km east / north of the radar centre */
} radar_rwy_t;

static radar_apt_t *s_radar_apt; /* PSRAM; most important first (large, then nearer) */
static radar_rwy_t *s_radar_rwy; /* PSRAM */
static int s_radar_apt_n;
static int s_radar_range_km = APP_CONFIG_RADAR_KM_DEFAULT; /* range of the radar on show (its location's setting) */

extern const uint8_t airports_bin_start[] asm("_binary_airports_bin_start");
extern const uint8_t airports_bin_end[] asm("_binary_airports_bin_end");

static int32_t rd_i32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, sizeof(v)); /* the embedded blob has no alignment guarantee */
    return v;
}

/* Collect the airports within s_radar_range_km of (lat0, lon0). */
static void radar_load_airports(double lat0, double lon0)
{
    s_radar_apt_n = 0;
    const uint8_t *blob = airports_bin_start;
    size_t size = (size_t)(airports_bin_end - airports_bin_start);
    if (size < 12 || memcmp(blob, "APT2", 4) != 0) {
        return;
    }
    uint32_t n_ap = (uint32_t)rd_i32(blob + 4);
    uint32_t n_rw = (uint32_t)rd_i32(blob + 8);
    if (12 + (size_t)n_ap * 20 + (size_t)n_rw * 16 > size) {
        return;
    }
    const uint8_t *ap = blob + 12;
    const uint8_t *rw = ap + (size_t)n_ap * 20;

    const float range = (float)s_radar_range_km;
    const double ky = 110.57;
    const double kx = 111.32 * cos(lat0 * M_PI / 180.0);
    const int32_t lat0_e4 = (int32_t)lround(lat0 * 1e4);
    const int32_t lon0_e4 = (int32_t)lround(lon0 * 1e4);
    const int32_t dlat_e4 = (int32_t)(range / ky * 1e4) + 10;
    const int32_t dlon_e4 = (int32_t)(range / kx * 1e4) + 10;

    /* Keep the RADAR_APT_MAX most important candidates: rank by class, then by
     * distance. `best` holds record indices in rank order. */
    uint32_t best[RADAR_APT_MAX];
    float best_key[RADAR_APT_MAX];
    int n_best = 0;
    for (uint32_t i = 0; i < n_ap; i++) {
        const uint8_t *rec = ap + (size_t)i * 20;
        int32_t lat = rd_i32(rec + 8), lon = rd_i32(rec + 12);
        if (abs(lat - lat0_e4) > dlat_e4 || abs(lon - lon0_e4) > dlon_e4) {
            continue;
        }
        float x = (float)((lon - lon0_e4) * 1e-4 * kx);
        float y = (float)((lat - lat0_e4) * 1e-4 * ky);
        float d2 = x * x + y * y;
        if (d2 > range * range) {
            continue;
        }
        float key = (float)rec[19] * 1e6f + d2; /* class 0=large .. 2=small */
        int at = n_best;
        if (n_best == RADAR_APT_MAX) {
            if (key >= best_key[n_best - 1]) {
                continue;
            }
            at = n_best - 1;
        } else {
            n_best++;
        }
        while (at > 0 && best_key[at - 1] > key) {
            best[at] = best[at - 1];
            best_key[at] = best_key[at - 1];
            at--;
        }
        best[at] = i;
        best_key[at] = key;
    }

    int n_rwy_used = 0;
    for (int b = 0; b < n_best; b++) {
        const uint8_t *rec = ap + (size_t)best[b] * 20;
        radar_apt_t *a = &s_radar_apt[b];
        /* IATA code (offset 4) if it has one, else the ICAO code (offset 0). */
        memcpy(a->label, rec + 4, 4);
        if (a->label[0] == ' ' || a->label[0] == '\0') {
            memcpy(a->label, rec, 4);
        }
        a->label[4] = '\0';
        for (int k = 3; k >= 0 && (a->label[k] == ' ' || a->label[k] == '\0'); k--) {
            a->label[k] = '\0';
        }
        a->x_km = (float)((rd_i32(rec + 12) - lon0_e4) * 1e-4 * kx);
        a->y_km = (float)((rd_i32(rec + 8) - lat0_e4) * 1e-4 * ky);
        uint16_t first;
        memcpy(&first, rec + 16, sizeof(first));
        int n = rec[18];
        if (n > RADAR_APT_RWY_MAX) {
            n = RADAR_APT_RWY_MAX;
        }
        a->first_rwy = (uint8_t)n_rwy_used;
        a->n_rwy = (uint8_t)n;
        for (int r = 0; r < n; r++) {
            const uint8_t *rr = rw + ((size_t)first + r) * 16;
            radar_rwy_t *o = &s_radar_rwy[n_rwy_used++];
            o->y1 = (float)((rd_i32(rr + 0) - lat0_e4) * 1e-4 * ky);
            o->x1 = (float)((rd_i32(rr + 4) - lon0_e4) * 1e-4 * kx);
            o->y2 = (float)((rd_i32(rr + 8) - lat0_e4) * 1e-4 * ky);
            o->x2 = (float)((rd_i32(rr + 12) - lon0_e4) * 1e-4 * kx);
        }
    }
    s_radar_apt_n = n_best;
    ESP_LOGI(TAG, "Radar: %d airport(s) within %d km", n_best, s_radar_range_km);
}

static void radar_dot(lv_layer_t *layer, int cx, int cy, int r, lv_color_t color)
{
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    if (!radar_area_hits_clip(layer, &a)) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = color;
    d.border_width = 0;
    lv_draw_rect(layer, &d, &a);
}

/* Runways (or a dot when they would be under a few pixels), under the aircraft. */
static void radar_draw_airports(lv_layer_t *layer, int range)
{
    const lv_color_t col = lv_color_hex(s_rp->apt);
    const float px_per_km = (float)RADAR_R / (float)range;
    for (int i = 0; i < s_radar_apt_n; i++) {
        const radar_apt_t *a = &s_radar_apt[i];
        int cx = RADAR_CX + (int)lroundf(a->x_km * px_per_km);
        int cy = RADAR_CY - (int)lroundf(a->y_km * px_per_km);
        float longest = 0.0f;
        for (int r = 0; r < a->n_rwy; r++) {
            const radar_rwy_t *w = &s_radar_rwy[a->first_rwy + r];
            float len = hypotf(w->x2 - w->x1, w->y2 - w->y1) * px_per_km;
            if (len > longest) {
                longest = len;
            }
            radar_line(layer,
                       RADAR_CX + (int)lroundf(w->x1 * px_per_km), RADAR_CY - (int)lroundf(w->y1 * px_per_km),
                       RADAR_CX + (int)lroundf(w->x2 * px_per_km), RADAR_CY - (int)lroundf(w->y2 * px_per_km),
                       2, col);
        }
        if (longest < 8.0f) {
            radar_dot(layer, cx, cy, 3, col);
        }
    }
}

/* ICAO labels for the airports, most important first, drawn after the aircraft
 * tags and skipped where they would land on a tag or on each other. `tags`
 * holds the rectangles already taken; new labels are appended to it. */
static void radar_draw_airport_labels(lv_layer_t *layer, int range, int lh, lv_area_t *tags, int *n_tags,
                                      int max_tags)
{
    const lv_color_t col = lv_color_hex(s_rp->apt);
    const float px_per_km = (float)RADAR_R / (float)range;
    for (int i = 0; i < s_radar_apt_n && *n_tags < max_tags; i++) {
        const radar_apt_t *a = &s_radar_apt[i];
        if (a->label[0] == '\0') {
            continue;
        }
        int cx = RADAR_CX + (int)lroundf(a->x_km * px_per_km);
        int cy = RADAR_CY - (int)lroundf(a->y_km * px_per_km);
        lv_point_t sz;
        lv_text_get_size(&sz, a->label, s_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

        /* To the right of the airport, or to the left if that would spill
         * out of the plot. */
        lv_area_t lab = { cx + 8, cy - lh / 2, 0, cy + lh / 2 };
        lab.x2 = lab.x1 + sz.x + 2;
        int dxr = lab.x2 - RADAR_CX, dyr = cy - RADAR_CY;
        if (dxr * dxr + dyr * dyr > RADAR_R * RADAR_R) {
            lab.x1 = cx - 8 - sz.x - 2;
            lab.x2 = lab.x1 + sz.x + 2;
        }
        bool clash = false;
        for (int k = 0; k < *n_tags; k++) {
            const lv_area_t *o = &tags[k];
            if (lab.x1 <= o->x2 && lab.x2 >= o->x1 && lab.y1 <= o->y2 && lab.y2 >= o->y1) {
                clash = true;
                break;
            }
        }
        if (clash) {
            continue;
        }
        tags[(*n_tags)++] = lab;
        radar_text(layer, a->label, lab.x1, lab.y1, sz.x + 2, LV_TEXT_ALIGN_LEFT, col);
    }
}

/* Draw `txt` left-aligned in w px, shortened with ".." if it doesn't fit. */
static void radar_text_fit(lv_layer_t *layer, const char *txt, int x, int y, int w, lv_color_t color)
{
    if (!radar_vis(layer, x, y, x + w - 1, y + lv_font_get_line_height(s_font_body) - 1)) {
        return;
    }
    char buf[sizeof(((ais_ship_t *)0)->name) + 2];
    size_t len = strlen(txt);
    if (len > sizeof(buf) - 3) {
        len = sizeof(buf) - 3;
    }
    for (size_t n = len;; n--) {
        memcpy(buf, txt, n);
        if (n < len) {
            memcpy(buf + n, "..", 3);
        } else {
            buf[n] = '\0';
        }
        lv_point_t sz;
        lv_text_get_size(&sz, buf, s_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= w || n <= 1) {
            break;
        }
    }
    radar_text(layer, buf, x, y, w, LV_TEXT_ALIGN_LEFT, color);
}

/* Where ship `s` is now on the plot, in px from the centre (y up): its reported
 * position moved along its course for the report's age, unless it's moored.
 * False if that is outside the outer ring. */
static bool ship_plot_pos(const ais_ship_t *s, int range, float since_fetch_s, float *sx, float *sy)
{
    const float deg = (float)M_PI / 180.0f;
    float x_km = s->dist_km * sinf(s->bearing_deg * deg);
    float y_km = s->dist_km * cosf(s->bearing_deg * deg);
    if (!s->moored) {
        float t = fminf(s->age_s + since_fetch_s, SHIP_EXTRAP_MAX_S);
        float moved_km = s->sog_kn * KM_PER_NM * t / 3600.0f;
        x_km += moved_km * sinf(s->cog_deg * deg);
        y_km += moved_km * cosf(s->cog_deg * deg);
    }
    *sx = x_km / (float)range * RADAR_R;
    *sy = y_km / (float)range * RADAR_R;
    return *sx * *sx + *sy * *sy <= (float)(RADAR_R * RADAR_R);
}

/* Ship traffic on the radar grid (already drawn): a hull-shaped marker along
 * each ship's heading with a SHIP_VEC_MIN-minute course vector, a dot for
 * moored / anchored ones, name tags for the nearest, and the table. */
static void ships_draw(lv_layer_t *layer, int range, int lh)
{
    const lv_color_t c_ring = lv_color_hex(s_rp->ring);
    const lv_color_t c_txt = lv_color_hex(s_rp->txt);
    const lv_color_t c_dim = lv_color_hex(s_rp->dim);

    radar_line(layer, RADAR_LIST_X - 10, 48, RADAR_LIST_X - 10, 470, 1, c_ring);
    radar_text(layer, "Navn", RADAR_LIST_X, 50, 136, LV_TEXT_ALIGN_LEFT, c_dim);
    radar_text(layer, "Type", RADAR_LIST_X + 136, 50, 58, LV_TEXT_ALIGN_LEFT, c_dim);
    radar_text(layer, "kn", RADAR_LIST_X + 194, 50, 44, LV_TEXT_ALIGN_RIGHT, c_dim);
    radar_text(layer, "km", RADAR_LIST_X + 238, 50, 56, LV_TEXT_ALIGN_RIGHT, c_dim);

    const ais_result_t *res = (s_radar_valid && s_ship_data != NULL) ? s_ship_data : NULL;
    if (res == NULL) {
        return;
    }

    lv_area_t tags[RADAR_TAGS];
    int n_tags = 0;
    const float since_fetch_s = (float)(lv_tick_get() - s_radar_tick) / 1000.0f;
    const float deg = (float)M_PI / 180.0f;

    /* Farthest first, so the nearest ships end up on top. */
    for (int i = res->count - 1; i >= 0; i--) {
        const ais_ship_t *s = &res->ship[i];
        const lv_color_t col = lv_color_hex(s_rp->ship[s->category < AIS_CAT_COUNT ? s->category : 0]);

        float sx, sy;
        if (!ship_plot_pos(s, range, since_fetch_s, &sx, &sy)) {
            continue;
        }
        int px = RADAR_CX + (int)lroundf(sx);
        int py = RADAR_CY - (int)lroundf(sy);

        if (s->moored) {
            radar_dot(layer, px, py, 3, col);
        } else {
            float hr = s->heading_deg * deg;
            float fx = sinf(hr), fy = -cosf(hr);
            float qx = -fy, qy = fx;
            int tri[3][2] = {
                { px + (int)lroundf(fx * 10), py + (int)lroundf(fy * 10) },
                { px + (int)lroundf(-fx * 6 + qx * 4), py + (int)lroundf(-fy * 6 + qy * 4) },
                { px + (int)lroundf(-fx * 6 - qx * 4), py + (int)lroundf(-fy * 6 - qy * 4) },
            };
            float vx = sinf(s->cog_deg * deg), vy = -cosf(s->cog_deg * deg);
            float vec_px = s->sog_kn * KM_PER_NM * SHIP_VEC_MIN / 60.0f / (float)range * RADAR_R;
            if (vec_px > 70.0f) {
                vec_px = 70.0f;
            }
            float b = sx * vx - sy * vy;
            float c = sx * sx + sy * sy - (float)(RADAR_R * RADAR_R);
            float t_max = -b + sqrtf(b * b - c);
            if (vec_px > t_max) {
                vec_px = t_max;
            }
            if (vec_px > 3.0f) {
                radar_line(layer, px, py, px + (int)lroundf(vx * vec_px), py + (int)lroundf(vy * vec_px),
                           1, col);
            }
            radar_triangle(layer, tri, col);
        }
    }

    /* Name tags for the nearest few, on the side facing the centre, skipped
     * where they'd overlap an earlier one. Computed in the same order on every
     * strip so the choice is consistent across the whole frame. */
    for (int i = 0; i < res->count && i < RADAR_TAGS; i++) {
        const ais_ship_t *s = &res->ship[i];
        float sx, sy;
        if (!ship_plot_pos(s, range, since_fetch_s, &sx, &sy)) {
            continue;
        }
        int px = RADAR_CX + (int)lroundf(sx);
        int py = RADAR_CY - (int)lroundf(sy);
        lv_point_t sz;
        lv_text_get_size(&sz, s->name, s_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int w = sz.x + 2 < SHIP_TAG_MAX_W ? sz.x + 2 : SHIP_TAG_MAX_W;
        lv_area_t tag = { (px < RADAR_CX) ? px + 10 : px - 10 - w, py - lh / 2, 0, py + lh / 2 };
        tag.x2 = tag.x1 + w;
        bool clash = false;
        for (int k = 0; k < n_tags; k++) {
            const lv_area_t *o = &tags[k];
            if (tag.x1 <= o->x2 && tag.x2 >= o->x1 && tag.y1 <= o->y2 && tag.y2 >= o->y1) {
                clash = true;
                break;
            }
        }
        if (!clash) {
            tags[n_tags++] = tag;
            radar_text_fit(layer, s->name, tag.x1, tag.y1, w, c_txt);
        }
    }

    if (res->count == 0) {
        char none[48];
        snprintf(none, sizeof(none), "Ingen skip innen %d km", range);
        radar_text(layer, none, RADAR_LIST_X, RADAR_LIST_Y, 290, LV_TEXT_ALIGN_LEFT, c_txt);
        return;
    }

    for (int i = 0; i < res->count && i < RADAR_LIST_ROWS; i++) {
        const ais_ship_t *s = &res->ship[i];
        const lv_color_t col = lv_color_hex(s_rp->ship[s->category < AIS_CAT_COUNT ? s->category : 0]);
        int y = RADAR_LIST_Y + i * RADAR_LIST_ROW_H;
        char kn[8], dist[8];
        if (s->moored) {
            snprintf(kn, sizeof(kn), "-");
        } else {
            snprintf(kn, sizeof(kn), "%.0f", (double)s->sog_kn);
        }
        snprintf(dist, sizeof(dist), s->dist_km < 10.0f ? "%.1f" : "%.0f", (double)s->dist_km);
        radar_text_fit(layer, s->name, RADAR_LIST_X, y, 132, c_txt);
        radar_text(layer, ais_category_label(s->category), RADAR_LIST_X + 136, y, 58,
                   LV_TEXT_ALIGN_LEFT, col);
        radar_text(layer, kn, RADAR_LIST_X + 194, y, 44, LV_TEXT_ALIGN_RIGHT, c_txt);
        radar_text(layer, dist, RADAR_LIST_X + 238, y, 56, LV_TEXT_ALIGN_RIGHT, c_txt);
    }
    if (res->total > res->count) {
        char more[40];
        snprintf(more, sizeof(more), "Viser %d av %d skip", res->count, res->total);
        radar_text(layer, more, RADAR_LIST_X, RADAR_LIST_Y + RADAR_LIST_ROWS * RADAR_LIST_ROW_H + 4,
                   290, LV_TEXT_ALIGN_LEFT, c_dim);
    }
}

static void radar_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_color_t c_disc = lv_color_hex(s_rp->disc);
    const lv_color_t c_ring = lv_color_hex(s_rp->ring);
    const lv_color_t c_txt = lv_color_hex(s_rp->txt);
    const lv_color_t c_dim = lv_color_hex(s_rp->dim);
    const lv_color_t c_plane = lv_color_hex(s_rp->plane);
    const lv_color_t c_vec = lv_color_hex(s_rp->vec);
    const int lh = lv_font_get_line_height(s_font_body);
    const int range = s_radar_range_km;

    /* Grid: disc, range rings at 1/4 steps, crosshair, centre dot - with the
     * coastline under the rings. */
    radar_circle(layer, RADAR_R, 2, c_ring, true, c_disc);
    if (s_coast_valid &&
        radar_vis(layer, RADAR_CX - RADAR_R, RADAR_CY - RADAR_R, RADAR_CX + RADAR_R, RADAR_CY + RADAR_R)) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        lv_area_t a = { RADAR_CX - RADAR_R, RADAR_CY - RADAR_R,
                        RADAR_CX - RADAR_R + COAST_D - 1, RADAR_CY - RADAR_R + COAST_D - 1 };
        d.src = &s_water_img;
        d.recolor = lv_color_hex(s_rp->water); /* the colour of an A8 image */
        lv_draw_image(layer, &d, &a);
        d.src = &s_coast_img;
        d.recolor = lv_color_hex(s_rp->coast);
        lv_draw_image(layer, &d, &a);
    }
    for (int i = 1; i < 4; i++) {
        radar_circle(layer, RADAR_R * i / 4, 1, c_ring, false, c_disc);
    }
    radar_line(layer, RADAR_CX - RADAR_R, RADAR_CY, RADAR_CX + RADAR_R, RADAR_CY, 1, c_ring);
    radar_line(layer, RADAR_CX, RADAR_CY - RADAR_R, RADAR_CX, RADAR_CY + RADAR_R, 1, c_ring);
    radar_circle(layer, 3, 0, c_txt, true, c_txt);

    /* Compass letters at the rim and the ring distances along the east spoke. */
    radar_text(layer, "N", RADAR_CX - 12, RADAR_CY - RADAR_R - lh + 1, 24, LV_TEXT_ALIGN_CENTER, c_txt);
    radar_text(layer, "S", RADAR_CX - 12, RADAR_CY + RADAR_R + 1, 24, LV_TEXT_ALIGN_CENTER, c_txt);
    radar_text(layer, "\xC3\x98", RADAR_CX + RADAR_R + 4, RADAR_CY - lh / 2, 24, LV_TEXT_ALIGN_LEFT, c_txt);
    radar_text(layer, "V", RADAR_CX - RADAR_R - 28, RADAR_CY - lh / 2, 24, LV_TEXT_ALIGN_RIGHT, c_txt);
    /* Range labels on every other ring: the second ring's number sits on the
     * east spoke; the outer ring's is lifted one line above the spoke (clear of
     * the "\xC3\x98" there), with the number just inside the ring and the unit
     * just outside it. */
    for (int i = 2; i <= 4; i += 2) {
        char num[12];
        float ring_km = range * i / 4.0f;
        if (ring_km == floorf(ring_km)) {
            snprintf(num, sizeof(num), "%d", (int)ring_km);
        } else {
            snprintf(num, sizeof(num), "%.1f", (double)ring_km);
        }
        if (i == 2) {
            radar_text(layer, num, RADAR_CX + RADAR_R * i / 4 - 70, RADAR_CY - lh - 1, 70,
                       LV_TEXT_ALIGN_RIGHT, c_dim);
        } else {
            int y = RADAR_CY - 2 * lh - 1;
            float dy = (float)(RADAR_CY - (y + lh / 2)); /* label centre above the spoke */
            int ring_x = RADAR_CX + (int)lroundf(sqrtf((float)(RADAR_R * RADAR_R) - dy * dy));
            radar_text(layer, num, ring_x - 4 - 60, y, 60, LV_TEXT_ALIGN_RIGHT, c_dim);
            radar_text(layer, "km", ring_x + 4, y, 34, LV_TEXT_ALIGN_LEFT, c_dim);
        }
    }

    if (s_coast_valid) {
        radar_text(layer, "\xC2\xA9 OpenStreetMap", 8, 480 - lh - 4, 200, LV_TEXT_ALIGN_LEFT, c_dim);
    }
    if (s_ship_mode) {
        ships_draw(layer, range, lh);
        return;
    }

    radar_draw_airports(layer, range);

    /* Table header + divider. */
    radar_line(layer, RADAR_LIST_X - 10, 48, RADAR_LIST_X - 10, 470, 1, c_ring);
    radar_text(layer, "Fly", RADAR_LIST_X, 50, 88, LV_TEXT_ALIGN_LEFT, c_dim);
    radar_text(layer, "Type", RADAR_LIST_X + 88, 50, 58, LV_TEXT_ALIGN_LEFT, c_dim);
    radar_text(layer, "H\xC3\xB8yde", RADAR_LIST_X + 146, 50, 72, LV_TEXT_ALIGN_RIGHT, c_dim);
    radar_text(layer, "kt", RADAR_LIST_X + 218, 50, 38, LV_TEXT_ALIGN_RIGHT, c_dim);
    radar_text(layer, "km", RADAR_LIST_X + 256, 50, 38, LV_TEXT_ALIGN_RIGHT, c_dim);

    const adsb_result_t *res = (s_radar_valid && s_radar_data != NULL) ? s_radar_data : NULL;

    lv_area_t tags[RADAR_TAGS + RADAR_APT_MAX];
    int n_tags = 0;
    const float age_s = (float)(lv_tick_get() - s_radar_tick) / 1000.0f;
    const float deg = (float)M_PI / 180.0f;

    for (int i = 0; res != NULL && i < res->count; i++) {
        const adsb_aircraft_t *a = &res->ac[i];

        /* Where it is now: its reported position moved along its track for the
         * time since the report (fetch age + the report's own age). */
        float br = a->bearing_deg * deg;
        float tr = a->track_deg * deg;
        float x_km = a->dist_km * sinf(br);
        float y_km = a->dist_km * cosf(br);
        float moved_km = a->gs_kt * KM_PER_NM * (age_s + a->seen_pos_s) / 3600.0f;
        x_km += moved_km * sinf(tr);
        y_km += moved_km * cosf(tr);

        float sx = x_km / (float)range * RADAR_R;
        float sy = y_km / (float)range * RADAR_R;
        if (sx * sx + sy * sy > (float)(RADAR_R * RADAR_R)) {
            continue; /* flown out of range since the report */
        }
        int px = RADAR_CX + (int)lroundf(sx);
        int py = RADAR_CY - (int)lroundf(sy);

        /* Heading triangle plus a 60-second speed vector ahead of it. */
        float fx = sinf(tr), fy = -cosf(tr); /* unit vector along the track, screen coords */
        float qx = -fy, qy = fx;             /* perpendicular */
        int tri[3][2] = {
            { px + (int)lroundf(fx * 9), py + (int)lroundf(fy * 9) },
            { px + (int)lroundf(-fx * 5 + qx * 5), py + (int)lroundf(-fy * 5 + qy * 5) },
            { px + (int)lroundf(-fx * 5 - qx * 5), py + (int)lroundf(-fy * 5 - qy * 5) },
        };
        float vec_px = a->gs_kt * KM_PER_NM / 60.0f / (float)range * RADAR_R;
        if (vec_px > 70.0f) {
            vec_px = 70.0f;
        }
        {
            /* Keep the vector inside the outer ring: distance along the track
             * from the aircraft to where it crosses the ring. */
            float b = sx * fx - sy * fy; /* p . f, with p in screen coordinates (y down) */
            float c = sx * sx + sy * sy - (float)(RADAR_R * RADAR_R);
            float t_max = -b + sqrtf(b * b - c) - 9.0f;
            if (vec_px > t_max) {
                vec_px = t_max;
            }
        }
        if (vec_px > 3.0f) {
            radar_line(layer, tri[0][0], tri[0][1],
                       px + (int)lroundf(fx * (9 + vec_px)), py + (int)lroundf(fy * (9 + vec_px)),
                       1, c_vec);
        }
        radar_triangle(layer, tri, c_plane);

        /* Callsign tag for the nearest few, on the side facing the centre;
         * skipped if it would land on a tag already placed. */
        if (i < RADAR_TAGS) {
            lv_point_t sz;
            lv_text_get_size(&sz, a->callsign, s_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            lv_area_t tag = { (px < RADAR_CX) ? px + 12 : px - 12 - sz.x, py - lh / 2, 0, py + lh / 2 };
            tag.x2 = tag.x1 + sz.x + 2;
            bool clash = false;
            for (int k = 0; k < n_tags; k++) {
                const lv_area_t *o = &tags[k];
                if (tag.x1 <= o->x2 && tag.x2 >= o->x1 && tag.y1 <= o->y2 && tag.y2 >= o->y1) {
                    clash = true;
                    break;
                }
            }
            if (!clash) {
                tags[n_tags++] = tag;
                radar_text(layer, a->callsign, tag.x1, tag.y1, sz.x + 2, LV_TEXT_ALIGN_LEFT, c_txt);
            }
        }
    }

    radar_draw_airport_labels(layer, range, lh, tags, &n_tags, RADAR_TAGS + RADAR_APT_MAX);

    if (res == NULL) {
        return; /* nothing fetched yet */
    }
    if (res->count == 0) {
        char none[48];
        snprintf(none, sizeof(none), "Ingen fly innen %d km", range);
        radar_text(layer, none, RADAR_LIST_X, RADAR_LIST_Y, 290, LV_TEXT_ALIGN_LEFT, c_txt);
        return;
    }

    /* Table of the nearest aircraft. */
    for (int i = 0; i < res->count && i < RADAR_LIST_ROWS; i++) {
        const adsb_aircraft_t *a = &res->ac[i];
        int y = RADAR_LIST_Y + i * RADAR_LIST_ROW_H;
        char alt[16], gs[8], dist[8];
        radar_fmt_alt(alt, sizeof(alt), a->alt_ft);
        snprintf(gs, sizeof(gs), "%d", (int)lroundf(a->gs_kt));
        snprintf(dist, sizeof(dist), a->dist_km < 10.0f ? "%.1f" : "%.0f", (double)a->dist_km);
        radar_text(layer, a->callsign, RADAR_LIST_X, y, 88, LV_TEXT_ALIGN_LEFT, c_txt);
        radar_text(layer, a->type, RADAR_LIST_X + 88, y, 58, LV_TEXT_ALIGN_LEFT, c_dim);
        radar_text(layer, alt, RADAR_LIST_X + 146, y, 72, LV_TEXT_ALIGN_RIGHT, c_txt);
        radar_text(layer, gs, RADAR_LIST_X + 218, y, 38, LV_TEXT_ALIGN_RIGHT, c_txt);
        radar_text(layer, dist, RADAR_LIST_X + 256, y, 38, LV_TEXT_ALIGN_RIGHT, c_txt);
    }
    if (res->total > res->count) {
        char more[40];
        snprintf(more, sizeof(more), "Viser %d av %d fly", res->count, res->total);
        radar_text(layer, more, RADAR_LIST_X, RADAR_LIST_Y + RADAR_LIST_ROWS * RADAR_LIST_ROW_H + 4,
                   290, LV_TEXT_ALIGN_LEFT, c_dim);
    }
}

/* The ship count line, e.g. "2 skip lengre enn 100 meter innen 20 km",
 * mentioning the length filter only when one is configured. */
static void ship_info_set(int total)
{
    const unsigned min_len = (s_radar_loc >= 0) ? s_cfg.ship_min_len_m[s_radar_loc] : 0;
    if (min_len > 0) {
        lv_label_set_text_fmt(s_radar_info, "%d skip lengre enn %u meter innen %u km",
                              total, min_len, (unsigned)s_radar_range_km);
    } else {
        lv_label_set_text_fmt(s_radar_info, "%d skip innen %u km", total, (unsigned)s_radar_range_km);
    }
}

/* Positions are extrapolated from each aircraft's speed and track, so repaint
 * periodically while the radar is on screen. Runs in the LVGL task. */
static void radar_redraw_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_radar_valid && s_radar_canvas != NULL && !lv_obj_has_flag(s_radar_root, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_invalidate(s_radar_canvas);
    }
}

/* Called with the adapter lock held. */
static void ships_apply(const ais_result_t *res)
{
    memcpy(s_ship_data, res, sizeof(*res));
    s_radar_valid = true;
    s_radar_tick = lv_tick_get();
    ship_info_set(res->total);
    lv_obj_invalidate(s_radar_canvas);
}

/* Called with the adapter lock held. */
static void radar_apply(const adsb_result_t *res)
{
    memcpy(s_radar_data, res, sizeof(*res));
    s_radar_valid = true;
    s_radar_tick = lv_tick_get();

    char info[64];
    time_t now = time(NULL);
    if (now > PLAUSIBLE_EPOCH_S) {
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf(info, sizeof(info), "%d fly innen %u km  kl. %02d:%02d",
                 res->total, (unsigned)s_radar_range_km, lt.tm_hour, lt.tm_min);
    } else {
        snprintf(info, sizeof(info), "%d fly innen %u km", res->total, (unsigned)s_radar_range_km);
    }
    lv_label_set_text(s_radar_info, info);
    lv_obj_align(s_radar_info, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_obj_invalidate(s_radar_canvas);
}

static void build_radar(lv_obj_t *root)
{
    s_radar_data = heap_caps_calloc(1, sizeof(*s_radar_data), MALLOC_CAP_SPIRAM);
    s_ship_data = heap_caps_calloc(1, sizeof(*s_ship_data), MALLOC_CAP_SPIRAM);
    s_coast_px = heap_caps_calloc(COAST_D, COAST_D, MALLOC_CAP_SPIRAM);
    s_coast_img = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,
                    .w = COAST_D, .h = COAST_D, .stride = COAST_D },
        .data = s_coast_px,
        .data_size = COAST_D * COAST_D,
    };
    s_water_px = heap_caps_calloc(COAST_D, COAST_D, MALLOC_CAP_SPIRAM);
    s_water_img = s_coast_img;
    s_water_seeds = heap_caps_malloc(WATER_SEEDS_MAX * sizeof(*s_water_seeds), MALLOC_CAP_SPIRAM);
    s_water_img.data = s_water_px;
    s_radar_apt = heap_caps_calloc(RADAR_APT_MAX, sizeof(*s_radar_apt), MALLOC_CAP_SPIRAM);
    s_radar_rwy = heap_caps_calloc(RADAR_APT_MAX * RADAR_APT_RWY_MAX, sizeof(*s_radar_rwy), MALLOC_CAP_SPIRAM);
    assert(s_radar_data != NULL && s_ship_data != NULL && s_coast_px != NULL && s_water_px != NULL && s_water_seeds != NULL &&
           s_radar_apt != NULL && s_radar_rwy != NULL);

    /* A screen of its own in the theme's radar palette. Text colour is
     * inherited by the labels below. */
    s_rp = (s_cfg.theme == APP_THEME_DARK) ? &RADAR_DARK : &RADAR_LIGHT;
    lv_obj_set_style_bg_color(root, lv_color_hex(s_rp->bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root, lv_color_hex(s_rp->txt), 0);

    s_radar_canvas = lv_obj_create(root);
    lv_obj_remove_style_all(s_radar_canvas);
    lv_obj_set_pos(s_radar_canvas, 0, 0);
    lv_obj_set_size(s_radar_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_radar_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_radar_canvas, radar_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_radar_title = lv_label_create(root);
    lv_obj_set_style_text_font(s_radar_title, s_font_large, 0);
    lv_obj_set_pos(s_radar_title, 12, 4);
    lv_label_set_text(s_radar_title, "");

    s_radar_info = lv_label_create(root);
    lv_obj_align(s_radar_info, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_label_set_text(s_radar_info, "");

    lv_timer_create(radar_redraw_timer_cb, RADAR_REDRAW_MS, NULL);
}

/* Point the radar at location `loc` (adapter lock held): its title, and the
 * airports within range. */
static void radar_set_location(int loc)
{
    s_ship_mode = false;
    lv_label_set_text_fmt(s_radar_title, "Fly n\xC3\xA6r %s", s_cfg.locations[loc].name);
    s_radar_range_km = s_cfg.radar_km[loc];
    coast_mark(loc, s_radar_range_km);
    radar_load_airports(atof(s_cfg.locations[loc].lat), atof(s_cfg.locations[loc].lon));
}

/* Point the radar screen at location `loc`'s ship traffic (adapter lock held). */
static void ships_set_location(int loc)
{
    s_ship_mode = true;
    lv_label_set_text_fmt(s_radar_title, "Skip n\xC3\xA6r %s", s_cfg.locations[loc].name);
    s_radar_range_km = s_cfg.ship_km[loc];
    coast_mark(loc, s_radar_range_km);
}

static void temp_segment(lv_layer_t *layer, float x1, float y1, float x2, float y2,
                         lv_color_t color)
{
    int w = TEMP_LINE_WIDTH;
    if (!radar_vis(layer, (int)(x1 < x2 ? x1 : x2) - w, (int)(y1 < y2 ? y1 : y2) - w,
                   (int)(x1 < x2 ? x2 : x1) + w, (int)(y1 < y2 ? y2 : y1) + w)) {
        return;
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = (int32_t)x1;
    d.p1.y = (int32_t)y1;
    d.p2.x = (int32_t)x2;
    d.p2.y = (int32_t)y2;
    d.width = w;
    d.color = color;
    d.opa = LV_OPA_COVER;
    d.round_start = 1;
    d.round_end = 1;
    lv_draw_line(layer, &d);
}

/* Draws the temperature line segment by segment: orange at or above 0 C and
 * blue below. A segment that crosses zero is split at the (linearly
 * interpolated) crossing point, so the colour changes exactly on 0 C. */
static void temp_line_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);

    for (uint32_t i = 0; i + 1 < s_temp_line_count; i++) {
        float v1 = s_temp_line_values[i], v2 = s_temp_line_values[i + 1];
        float x1 = area.x1 + s_temp_line_points[i].x, y1 = area.y1 + s_temp_line_points[i].y;
        float x2 = area.x1 + s_temp_line_points[i + 1].x, y2 = area.y1 + s_temp_line_points[i + 1].y;
        bool cold1 = v1 < 0.0f, cold2 = v2 < 0.0f;
        if (cold1 == cold2) {
            temp_segment(layer, x1, y1, x2, y2, cold1 ? s_temp_cold_color : s_temp_warm_color);
            continue;
        }
        float t = v1 / (v1 - v2);
        float xm = x1 + t * (x2 - x1), ym = y1 + t * (y2 - y1);
        temp_segment(layer, x1, y1, xm, ym, cold1 ? s_temp_cold_color : s_temp_warm_color);
        temp_segment(layer, xm, ym, x2, y2, cold2 ? s_temp_cold_color : s_temp_warm_color);
    }
}

/* Rounded caps and the line width poke slightly outside the chart box. */
static void temp_line_ext_size_cb(lv_event_t *e)
{
    int32_t *s = lv_event_get_param(e);
    if (*s < TEMP_LINE_WIDTH) {
        *s = TEMP_LINE_WIDTH;
    }
}

static void build_ui(lv_obj_t *screen)
{
    const bool dark = (s_cfg.theme == APP_THEME_DARK);
    lv_display_t *disp = lv_obj_get_display(screen);
    lv_display_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                                                     lv_palette_main(LV_PALETTE_RED), dark, s_font_body));
    lv_theme_apply(screen);

    lv_obj_set_style_text_font(screen, s_font_body, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Two full-screen transparent layers, one per view; only one is ever
     * visible. Every detail widget below is created inside s_detail_root. */
    s_detail_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_detail_root);
    lv_obj_set_pos(s_detail_root, 0, 0);
    lv_obj_set_size(s_detail_root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_detail_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_overview_root = lv_obj_create(screen);
    lv_obj_remove_style_all(s_overview_root);
    lv_obj_set_pos(s_overview_root, 0, 0);
    lv_obj_set_size(s_overview_root, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_overview_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* Centre text in every overview cell by inheritance - avoids a per-label
     * style property on ~30 widgets, which matters for internal DRAM. */
    lv_obj_set_style_text_align(s_overview_root, LV_TEXT_ALIGN_CENTER, 0);

    /* If s_view_index was restored (see app_config_save_last_view) to a
     * specific location's screen, show its name from the first frame rather
     * than always location 0's - the weather task corrects this itself
     * moments later regardless, once WiFi is up. */
    const view_stop_t *initial_stop = &s_stops[s_view_index];
    int initial_loc = (initial_stop->kind == STOP_OVERVIEW) ? 0 : initial_stop->loc;

    s_location_label = lv_label_create(s_detail_root);
    lv_obj_set_style_text_font(s_location_label, s_font_large, 0);
    lv_obj_set_pos(s_location_label, 12, 4);
    lv_label_set_text(s_location_label, s_cfg.locations[initial_loc].name);

    s_updated_label = lv_label_create(s_detail_root);
    lv_obj_align(s_updated_label, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_label_set_text(s_updated_label, "");

    /* The selected location's worst active severe weather alert, if any (see
     * update_alert_banner). Sits centred in the gap between the location name
     * and the "updated" timestamp: black text on a solid fill of the alert's
     * own colour, for contrast against the screen background - plain
     * coloured text there was hard to read. Hidden (not just empty) when
     * nothing is active, since a background-filled label would otherwise
     * still show as a blank coloured box. */
    s_alert_label = lv_label_create(s_detail_root);
    lv_obj_set_width(s_alert_label, 380);
    lv_label_set_long_mode(s_alert_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_alert_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_alert_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_alert_label, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_alert_label, 4, 0);
    lv_obj_set_style_pad_hor(s_alert_label, 10, 0);
    lv_obj_set_style_pad_ver(s_alert_label, 3, 0);
    lv_obj_align(s_alert_label, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_text(s_alert_label, "");
    lv_obj_add_flag(s_alert_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *icon_row = lv_obj_create(s_detail_root);
    lv_obj_set_pos(icon_row, CHART_X, ICON_ROW_Y);
    lv_obj_set_size(icon_row, CHART_W, ICON_SIZE);
    lv_obj_set_style_border_width(icon_row, 0, 0);
    lv_obj_set_style_pad_all(icon_row, 0, 0);
    lv_obj_set_style_bg_opa(icon_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(icon_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(icon_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < NUM_HOUR_LABELS; i++) {
        s_icon_slots[i] = lv_image_create(icon_row);
        lv_obj_set_size(s_icon_slots[i], ICON_SIZE, ICON_SIZE);
        lv_image_set_inner_align(s_icon_slots[i], LV_IMAGE_ALIGN_CENTER);
        lv_obj_add_flag(s_icon_slots[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Precipitation-max bar chart acts as the single visual chart frame
     * (background, border, gridlines) for the whole precip+temp area - the
     * min bars and the temperature line are both overlaid transparently on
     * top of it (same "frame widget + transparent overlay" trick as the
     * wind/gust chart pair below). Its bars are the high end of MET's
     * forecast uncertainty range for that hour, drawn taller and paler
     * "behind" the min bars in front - see update_ui_with_forecast. */
    s_precip_max_chart = lv_chart_create(s_detail_root);
    lv_obj_set_pos(s_precip_max_chart, CHART_X, CHART_Y);
    lv_obj_set_size(s_precip_max_chart, CHART_W, CHART_H);
    lv_obj_set_style_pad_left(s_precip_max_chart, 4, 0);
    lv_obj_set_style_pad_right(s_precip_max_chart, 4, 0);
    lv_chart_set_type(s_precip_max_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(s_precip_max_chart, 4, NUM_HOUR_LABELS - 1);
    lv_chart_set_point_count(s_precip_max_chart, YR_FORECAST_MAX_POINTS);
    lv_chart_set_axis_range(s_precip_max_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
    /* "Paler" than the main bars means lighter on a light background, darker
     * on a dark one; the same goes for the gust bars below. */
    s_precip_max_series = lv_chart_add_series(s_precip_max_chart,
                                              dark ? lv_palette_darken(LV_PALETTE_BLUE, 3)
                                                   : lv_palette_lighten(LV_PALETTE_BLUE, 3),
                                              LV_CHART_AXIS_PRIMARY_Y);

    /* Precipitation-min bar chart: the low end of the same range, in front -
     * its own background/border are transparent so the max chart behind it
     * shows through above wherever this (shorter, since max >= min) bar
     * doesn't reach. */
    s_precip_chart = lv_chart_create(s_detail_root);
    lv_obj_set_pos(s_precip_chart, CHART_X, CHART_Y);
    lv_obj_set_size(s_precip_chart, CHART_W, CHART_H);
    lv_obj_set_style_pad_left(s_precip_chart, 4, 0);
    lv_obj_set_style_pad_right(s_precip_chart, 4, 0);
    lv_obj_set_style_bg_opa(s_precip_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_precip_chart, 0, 0);
    lv_chart_set_type(s_precip_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_precip_chart, YR_FORECAST_MAX_POINTS);
    lv_chart_set_axis_range(s_precip_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
    s_precip_series = lv_chart_add_series(s_precip_chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);

    /* A plain transparent object painted by temp_line_draw_cb rather than an
     * lv_line, which can only draw in a single colour. The points are set each
     * refresh in update_ui_with_forecast. */
    s_temp_warm_color = lv_palette_main(LV_PALETTE_ORANGE);
    s_temp_cold_color = dark ? lv_palette_lighten(LV_PALETTE_BLUE, 2)
                             : lv_palette_darken(LV_PALETTE_BLUE, 2);
    s_temp_line = lv_obj_create(s_detail_root);
    lv_obj_remove_style_all(s_temp_line);
    lv_obj_set_pos(s_temp_line, CHART_X, CHART_Y);
    lv_obj_set_size(s_temp_line, CHART_W, CHART_H);
    lv_obj_clear_flag(s_temp_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_temp_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_temp_line, temp_line_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_temp_line, temp_line_ext_size_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);

    /* Value markers for temperature and precipitation extrema, positioned
     * directly on the chart each refresh. Both are pools: how many are used
     * depends on the forecast (see place_temp_markers / place_precip_markers).
     * Any left over are kept hidden. */
    for (int i = 0; i < TEMP_MARKER_POOL; i++) {
        s_temp_markers[i] = lv_label_create(s_detail_root);
        lv_obj_set_style_text_color(s_temp_markers[i],
                                    dark ? lv_palette_lighten(LV_PALETTE_ORANGE, 2)
                                         : lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);
        lv_label_set_text(s_temp_markers[i], "");
        lv_obj_add_flag(s_temp_markers[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < PRECIP_MARKER_POOL; i++) {
        s_precip_markers[i] = lv_label_create(s_detail_root);
        lv_obj_set_style_text_color(s_precip_markers[i],
                                    dark ? lv_palette_lighten(LV_PALETTE_BLUE, 2)
                                         : lv_palette_darken(LV_PALETTE_BLUE, 2), 0);
        lv_label_set_text(s_precip_markers[i], "");
        lv_obj_add_flag(s_precip_markers[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Wind direction: a row of arrows (one per sampled column) rotated to
     * point the way the wind blows, sitting just above the wind-speed chart. */
    lv_obj_t *wind_dir_row = lv_obj_create(s_detail_root);
    lv_obj_set_pos(wind_dir_row, CHART_X, WIND_DIR_ROW_Y);
    lv_obj_set_size(wind_dir_row, CHART_W, WIND_ARROW_SIZE);
    lv_obj_set_style_border_width(wind_dir_row, 0, 0);
    lv_obj_set_style_pad_all(wind_dir_row, 0, 0);
    lv_obj_set_style_bg_opa(wind_dir_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(wind_dir_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wind_dir_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(wind_dir_row, LV_OBJ_FLAG_SCROLLABLE);

    /* One shared style rather than per-arrow local styles (internal DRAM):
     * the arrow artwork is dark blue, too dim on the dark background. */
    static lv_style_t arrow_dark;
    if (dark) {
        lv_style_init(&arrow_dark);
        lv_style_set_image_recolor(&arrow_dark, lv_palette_lighten(LV_PALETTE_BLUE, 2));
        lv_style_set_image_recolor_opa(&arrow_dark, LV_OPA_COVER);
    }
    for (int i = 0; i < NUM_HOUR_LABELS; i++) {
        s_wind_dir_arrows[i] = lv_image_create(wind_dir_row);
        lv_obj_set_size(s_wind_dir_arrows[i], WIND_ARROW_SIZE, WIND_ARROW_SIZE);
        lv_image_set_src(s_wind_dir_arrows[i], "F:arrow.png");
        if (dark) {
            lv_obj_add_style(s_wind_dir_arrows[i], &arrow_dark, 0);
        }
        lv_image_set_inner_align(s_wind_dir_arrows[i], LV_IMAGE_ALIGN_CENTER);
        lv_image_set_pivot(s_wind_dir_arrows[i], WIND_ARROW_SIZE / 2, WIND_ARROW_SIZE / 2);
        lv_obj_add_flag(s_wind_dir_arrows[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Wind-gust bar chart (m/s): the plain bars-only layer behind the wind
     * chart below (see s_gust_chart's declaration). Same position/size/scale
     * as the wind chart, kept in sync every refresh. */
    s_gust_chart = lv_chart_create(s_detail_root);
    lv_obj_set_pos(s_gust_chart, CHART_X, WIND_CHART_Y);
    lv_obj_set_size(s_gust_chart, CHART_W, WIND_CHART_H);
    lv_obj_set_style_pad_left(s_gust_chart, 4, 0);
    lv_obj_set_style_pad_right(s_gust_chart, 4, 0);
    lv_obj_set_style_bg_opa(s_gust_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_gust_chart, 0, 0);
    lv_chart_set_type(s_gust_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_gust_chart, YR_FORECAST_MAX_POINTS);
    lv_chart_set_axis_range(s_gust_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    s_gust_series = lv_chart_add_series(s_gust_chart,
                                        dark ? lv_palette_darken(LV_PALETTE_TEAL, 3)
                                             : lv_palette_lighten(LV_PALETTE_TEAL, 3),
                                        LV_CHART_AXIS_PRIMARY_Y);

    /* Wind-speed bar chart (m/s), same x-scale as the main chart above. Its
     * own background is transparent so the gust chart behind it shows
     * through above wherever its (shorter, since gusts are >= sustained
     * wind) bar doesn't reach. */
    s_wind_chart = lv_chart_create(s_detail_root);
    lv_obj_set_pos(s_wind_chart, CHART_X, WIND_CHART_Y);
    lv_obj_set_size(s_wind_chart, CHART_W, WIND_CHART_H);
    lv_obj_set_style_pad_left(s_wind_chart, 4, 0);
    lv_obj_set_style_pad_right(s_wind_chart, 4, 0);
    lv_obj_set_style_bg_opa(s_wind_chart, LV_OPA_TRANSP, 0);
    lv_chart_set_type(s_wind_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(s_wind_chart, 2, NUM_HOUR_LABELS - 1);
    lv_chart_set_point_count(s_wind_chart, YR_FORECAST_MAX_POINTS);
    lv_chart_set_axis_range(s_wind_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    s_wind_series = lv_chart_add_series(s_wind_chart, lv_palette_main(LV_PALETTE_TEAL),
                                       LV_CHART_AXIS_PRIMARY_Y);

    /* Wind/gust value markers - see place_wind_markers. Sit near the chart
     * top, in the wind bar's colour. */
    for (int i = 0; i < WIND_MARKER_POOL; i++) {
        s_wind_markers[i] = lv_label_create(s_detail_root);
        lv_obj_set_style_text_color(s_wind_markers[i],
                                    dark ? lv_palette_lighten(LV_PALETTE_TEAL, 2)
                                         : lv_palette_darken(LV_PALETTE_TEAL, 2), 0);
        lv_label_set_text(s_wind_markers[i], "");
        lv_obj_add_flag(s_wind_markers[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *hour_row = lv_obj_create(s_detail_root);
    lv_obj_set_pos(hour_row, CHART_X, HOUR_ROW_Y);
    lv_obj_set_size(hour_row, CHART_W, 24);
    lv_obj_set_style_border_width(hour_row, 0, 0);
    lv_obj_set_style_pad_all(hour_row, 0, 0);
    lv_obj_set_style_bg_opa(hour_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(hour_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hour_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hour_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < NUM_HOUR_LABELS; i++) {
        s_hour_labels[i] = lv_label_create(hour_row);
        lv_label_set_text(s_hour_labels[i], "");
    }

    /* The overview table is always a cycling stop (see build_stops). */
    build_overview(s_overview_root);

    /* The radar screen exists only if some location has it enabled. */
    if (s_any_radar) {
        s_radar_root = lv_obj_create(screen);
        lv_obj_remove_style_all(s_radar_root);
        lv_obj_set_pos(s_radar_root, 0, 0);
        lv_obj_set_size(s_radar_root, LV_PCT(100), LV_PCT(100));
        lv_obj_clear_flag(s_radar_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        build_radar(s_radar_root);
        if (initial_stop->kind == STOP_RADAR) {
            radar_set_location(initial_loc);
        } else if (initial_stop->kind == STOP_SHIPS) {
            ships_set_location(initial_loc);
        }
    }

    /* Created after both view layers so it sits on top of whichever is shown:
     * loading/error text, centered over the screen until data lands. */
    s_status_label = lv_label_create(screen);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_status_label, "Kobler til WiFi...");

    /* Full-screen tap catcher. In LVGL 9 every lv_obj/lv_chart is clickable by
     * default, so a tap lands on whichever chart or row widget covers that
     * point and never reaches the screen. This overlay is the topmost child,
     * so it catches every tap anywhere on screen. It doubles as the night
     * dimming overlay (see yr_weather_task): the backlight on this board is
     * switched on/off through an I2C GPIO expander with no PWM output, so it
     * can't be dimmed in hardware - a translucent black layer over the
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

    /* Start on whichever screen s_view_index was restored to (the first stop
     * unless another was showing before the previous reboot). */
    show_view((stop_kind_t)initial_stop->kind);
}

/* Centers label horizontally on chart_x (absolute) and places it either
 * above or below chart_y (absolute), clamped to stay within the chart's
 * horizontal bounds. */
static void place_marker_label(lv_obj_t *label, int32_t chart_x, int32_t chart_y, bool above)
{
    lv_obj_update_layout(label);
    int32_t w = lv_obj_get_width(label);
    int32_t h = lv_obj_get_height(label);

    int32_t x = chart_x - w / 2;
    if (x < CHART_X) {
        x = CHART_X;
    } else if (x > CHART_X + CHART_W - w) {
        x = CHART_X + CHART_W - w;
    }

    int32_t y = above ? (chart_y - h - 2) : (chart_y + 2);
    lv_obj_set_pos(label, x, y);
}

static float pt_temp(const yr_forecast_point_t *p) { return p->air_temperature_c; }
static float pt_precip_max(const yr_forecast_point_t *p) { return p->precipitation_max_mm; }
static float pt_wind(const yr_forecast_point_t *p) { return p->wind_speed_ms; }
static float pt_wind_gust(const yr_forecast_point_t *p) { return p->wind_speed_of_gust_ms; }

/* A label is about to be placed at points[idx]. Look ahead one whole spacing
 * window (MARKER_MIN_GAP_H hours) and, if a stronger local extremum of the
 * same kind (higher for a max, lower for a min) sits in it, return that index
 * instead. Because the caller then jumps past the returned index, this
 * collapses a cluster of small wiggles - or a lesser peak sitting just before
 * the real one - into a single label on the true peak/trough. */
static int snap_to_better_extremum(const yr_forecast_t *fc, int idx, bool want_max,
                                   float (*get)(const yr_forecast_point_t *))
{
    int best = idx;
    float best_val = get(&fc->points[idx]);
    int64_t limit = fc->points[idx].epoch_utc + (int64_t)MARKER_MIN_GAP_H * 3600;

    for (int j = idx + 1; j < fc->point_count && fc->points[j].epoch_utc <= limit; j++) {
        float v = get(&fc->points[j]);
        float prev = get(&fc->points[j - 1]);
        float next = (j + 1 < fc->point_count) ? get(&fc->points[j + 1]) : v;
        bool is_max = (v > prev && v >= next);
        bool is_min = (v < prev && v <= next);
        if (want_max ? (is_max && v > best_val) : (is_min && v < best_val)) {
            best = j;
            best_val = v;
        }
    }
    return best;
}

/* How far points[idx] stands out as an extremum of the given type: walk out
 * each side (up to MARKER_MIN_GAP_H hours) until the series climbs back above
 * (for a max) or drops back below (for a min) points[idx], tracking the
 * turning point reached on each side; the prominence is the height above the
 * higher bounding valley (a max) or the depth below the lower bounding peak
 * (a min). A shallow wiggle sitting next to a strong opposite extremum scores
 * near zero. */
static float temp_prominence(const yr_forecast_t *fc, int idx, bool want_max)
{
    const float t = fc->points[idx].air_temperature_c;
    const int64_t lo = fc->points[idx].epoch_utc - (int64_t)MARKER_MIN_GAP_H * 3600;
    const int64_t hi = fc->points[idx].epoch_utc + (int64_t)MARKER_MIN_GAP_H * 3600;
    float left = t, right = t;

    for (int j = idx - 1; j >= 0 && fc->points[j].epoch_utc >= lo; j--) {
        float v = fc->points[j].air_temperature_c;
        if (want_max ? (v > t) : (v < t)) break;
        if (want_max ? (v < left) : (v > left)) left = v;
    }
    for (int j = idx + 1; j < fc->point_count && fc->points[j].epoch_utc <= hi; j++) {
        float v = fc->points[j].air_temperature_c;
        if (want_max ? (v > t) : (v < t)) break;
        if (want_max ? (v < right) : (v > right)) right = v;
    }
    return want_max ? (t - (left > right ? left : right))
                    : ((left < right ? left : right) - t);
}

/* Emit one temperature marker at points[m] (above = label over the line, a
 * max; else under it, a min), recording its epoch so later markers can space
 * themselves against it. No-op once the pool is full. */
static void temp_emit(const yr_forecast_t *fc, int m, bool above, int *used,
                      int64_t placed_max[], int *n_max,
                      int64_t placed_min[], int *n_min)
{
    if (*used >= TEMP_MARKER_POOL) {
        return;
    }
    lv_obj_t *label = s_temp_markers[(*used)++];
    lv_label_set_text_fmt(label, "%.1f\xC2\xB0", (double)fc->points[m].air_temperature_c);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    place_marker_label(label, CHART_X + s_temp_line_points[m].x,
                       CHART_Y + s_temp_line_points[m].y, above);
    if (above) {
        placed_max[(*n_max)++] = fc->points[m].epoch_utc;
    } else {
        placed_min[(*n_min)++] = fc->points[m].epoch_utc;
    }
}

/* Place the temperature value markers. The global high and low - and the
 * leftmost "now" point - are labelled first so they are never crowded out by
 * lesser extrema. A left-to-right pass then adds other local extrema, each
 * kept at least MARKER_MIN_GAP_H hours from every already-placed marker OF THE
 * SAME TYPE and required to be prominent enough
 * (temp_prominence >= MARKER_TEMP_MIN_SWING) to be worth a number; each is
 * consolidated onto the strongest same-type extremum in the window ahead
 * (snap_to_better_extremum). Unused pool labels are hidden. */
static void place_temp_markers(const yr_forecast_t *fc, int temp_min_idx, int temp_max_idx)
{
    int used = 0, n_max = 0, n_min = 0;
    int64_t placed_max[TEMP_MARKER_POOL];
    int64_t placed_min[TEMP_MARKER_POOL];

    /* 1. Globals and "now" - unconditionally. */
    temp_emit(fc, temp_max_idx, true, &used, placed_max, &n_max, placed_min, &n_min);
    if (temp_min_idx != temp_max_idx) {
        temp_emit(fc, temp_min_idx, false, &used, placed_max, &n_max, placed_min, &n_min);
    }
    if (fc->point_count > 1 && temp_max_idx != 0 && temp_min_idx != 0) {
        bool now_above = fc->points[0].air_temperature_c > fc->points[1].air_temperature_c;
        temp_emit(fc, 0, now_above, &used, placed_max, &n_max, placed_min, &n_min);
    }

    /* 2. Other local extrema, spaced per type and filtered by prominence. */
    for (int i = 1; i < fc->point_count - 1 && used < TEMP_MARKER_POOL; i++) {
        if (i == temp_max_idx || i == temp_min_idx) {
            continue;
        }
        float prev = fc->points[i - 1].air_temperature_c;
        float t = fc->points[i].air_temperature_c;
        float next = fc->points[i + 1].air_temperature_c;
        bool above = (t > prev && t >= next);
        bool below = (t < prev && t <= next);
        if (!above && !below) {
            continue;
        }

        int m = snap_to_better_extremum(fc, i, above, pt_temp);
        int64_t ep = fc->points[m].epoch_utc;

        const int64_t *arr = above ? placed_max : placed_min;
        int n = above ? n_max : n_min;
        bool too_close = false;
        for (int k = 0; k < n; k++) {
            int64_t d = ep - arr[k];
            if ((d < 0 ? -d : d) < (int64_t)MARKER_MIN_GAP_H * 3600) {
                too_close = true;
                break;
            }
        }

        if (!too_close && temp_prominence(fc, m, above) >= MARKER_TEMP_MIN_SWING) {
            temp_emit(fc, m, above, &used, placed_max, &n_max, placed_min, &n_min);
        }
        if (m > i) {
            i = m; /* skip past the span we consolidated across */
        }
    }

    for (int i = used; i < TEMP_MARKER_POOL; i++) {
        lv_obj_add_flag(s_temp_markers[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Pick up to max_count indices of the highest get() values in the forecast
 * (skipping values <= 0 - calm/dry, or for gust, an hour MET didn't forecast
 * one for at all). Each pick must be more than PEAK_LABEL_MIN_GAP_H hours
 * from every index already picked, so a second, lesser peak sitting right
 * next to a stronger one is treated as the same event and dropped rather
 * than double-labelled - the next pick is then whichever remaining point is
 * genuinely the next-highest and far enough away, or none at all. Returns
 * the count written to out_idx. */
static int pick_top_peaks(const yr_forecast_t *fc, float (*get)(const yr_forecast_point_t *),
                          int max_count, int *out_idx)
{
    int n = 0;
    for (int pick = 0; pick < max_count; pick++) {
        int best = -1;
        float best_val = 0.0f;
        for (int i = 0; i < fc->point_count; i++) {
            float v = get(&fc->points[i]);
            if (v <= best_val) {
                continue;
            }
            bool too_close = false;
            for (int k = 0; k < n; k++) {
                int64_t d = fc->points[i].epoch_utc - fc->points[out_idx[k]].epoch_utc;
                if ((d < 0 ? -d : d) < (int64_t)PEAK_LABEL_MIN_GAP_H * 3600) {
                    too_close = true;
                    break;
                }
            }
            if (too_close) {
                continue;
            }
            best_val = v;
            best = i;
        }
        if (best < 0) {
            break;
        }
        out_idx[n++] = best;
    }
    return n;
}

/* Precipitation value markers: up to the two highest max-precipitation peaks
 * (pick_top_peaks - deduplicated within PEAK_LABEL_MIN_GAP_H hours, so a
 * second peak less than 6h from the strongest is dropped rather than shown),
 * each labelled with its range, e.g. "0.5-2.3 mm" (the max chart's own
 * height, since it's always >= the min chart's - see s_precip_max_chart). */
static void place_precip_markers(const yr_forecast_t *fc, int32_t precip_range_max)
{
    int32_t precip_axis_max = precip_range_max * PRECIP_AXIS_COMPRESSION;
    int idx[PRECIP_MARKER_POOL];
    int n = pick_top_peaks(fc, pt_precip_max, PRECIP_MARKER_POOL, idx);

    for (int k = 0; k < n; k++) {
        int m = idx[k];
        int32_t x = (fc->point_count > 1) ? (int32_t)m * (CHART_W - 1) / (fc->point_count - 1) : 0;
        int32_t y = CHART_H - (int32_t)(((float)s_precip_max_chart_data[m] / (float)precip_axis_max) * CHART_H);

        lv_obj_t *label = s_precip_markers[k];
        lv_label_set_text_fmt(label, "%.1f\xE2\x80\x93%.1f mm", (double)fc->points[m].precipitation_min_mm,
                              (double)fc->points[m].precipitation_max_mm);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        place_marker_label(label, CHART_X + x, CHART_Y + y, true);
    }
    for (int i = n; i < PRECIP_MARKER_POOL; i++) {
        lv_obj_add_flag(s_precip_markers[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Wind/gust value markers: up to the two highest gust peaks (pick_top_peaks
 * - deduplicated within PEAK_LABEL_MIN_GAP_H hours, so a second peak less
 * than 6h from the strongest is dropped rather than shown), each labelled
 * with its sustained wind speed followed by its gust speed in parentheses,
 * e.g. "12 (18) m/s". Pinned near the chart's top rather than at the bar's
 * own (value-dependent) height, since a bottom-of-chart label was too easy
 * to miss. */
static void place_wind_markers(const yr_forecast_t *fc)
{
    int gust_idx[WIND_MARKER_POOL];
    int n = pick_top_peaks(fc, pt_wind_gust, WIND_MARKER_POOL, gust_idx);

    for (int k = 0; k < n; k++) {
        int m = gust_idx[k];
        int32_t x = (fc->point_count > 1) ? (int32_t)m * (CHART_W - 1) / (fc->point_count - 1) : 0;
        lv_obj_t *label = s_wind_markers[k];
        lv_label_set_text_fmt(label, "%.0f (%.0f) m/s", (double)fc->points[m].wind_speed_ms,
                              (double)fc->points[m].wind_speed_of_gust_ms);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        place_marker_label(label, CHART_X + x, WIND_CHART_Y, false);
    }
    for (int i = n; i < WIND_MARKER_POOL; i++) {
        lv_obj_add_flag(s_wind_markers[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Format a UTC epoch as "HH:00" local time, rounded to the nearest whole
 * hour, for the x-axis labels. The merged series is non-uniform in time
 * (10-min nowcast steps, then hourly), so an evenly-sampled point rarely
 * lands on the hour - showing its rounded hour keeps the axis readable.
 * Oslo's UTC offset is a whole number of hours, so rounding the epoch is
 * equivalent to rounding the local clock. */
static void format_hour_label(int64_t epoch, char *out, size_t out_len)
{
    time_t rounded = (time_t)(((epoch + 1800) / 3600) * 3600);
    struct tm lt;
    localtime_r(&rounded, &lt);
    snprintf(out, out_len, "%02d:00", lt.tm_hour);
}

static void update_ui_with_forecast(const yr_forecast_t *fc)
{
    const yr_forecast_point_t *now = &fc->points[0];

    lv_label_set_text_fmt(s_updated_label, "Oppdatert kl. %s", fc->updated_hour_minute);

    int temp_min_idx = 0, temp_max_idx = 0;
    float temp_min = now->air_temperature_c;
    float temp_max = now->air_temperature_c;
    float precip_max = 0.0f; /* the high end of the range - see s_precip_max_chart */
    float wind_max = 0.0f;
    float gust_max = 0.0f; /* folded into the shared wind/gust axis range below */

    for (int i = 0; i < fc->point_count; i++) {
        const yr_forecast_point_t *p = &fc->points[i];
        if (p->air_temperature_c < temp_min) {
            temp_min = p->air_temperature_c;
            temp_min_idx = i;
        }
        if (p->air_temperature_c > temp_max) {
            temp_max = p->air_temperature_c;
            temp_max_idx = i;
        }
        if (p->precipitation_max_mm > precip_max) {
            precip_max = p->precipitation_max_mm;
        }
        if (p->wind_speed_ms > wind_max) {
            wind_max = p->wind_speed_ms;
        }
        if (p->wind_speed_of_gust_ms > gust_max) {
            gust_max = p->wind_speed_of_gust_ms;
        }

        /* A dry hour draws no bar at all (LV_CHART_POINT_NONE), rather than a
         * flat zero-height stub sitting on the axis. */
        int32_t precip_min_tenths = round_to_int(p->precipitation_min_mm * 10.0f);
        s_precip_chart_data[i] = (precip_min_tenths > 0) ? precip_min_tenths : LV_CHART_POINT_NONE;
        int32_t precip_max_tenths = round_to_int(p->precipitation_max_mm * 10.0f);
        s_precip_max_chart_data[i] = (precip_max_tenths > 0) ? precip_max_tenths : LV_CHART_POINT_NONE;

        s_wind_chart_data[i] = round_to_int(p->wind_speed_ms * 10.0f);
        /* No bar (rather than a misleadingly flat one) for the far-out points
         * MET doesn't forecast a gust for at all - see wind_speed_of_gust_ms. */
        s_gust_chart_data[i] = (p->wind_speed_of_gust_ms > 0.0f)
                                    ? round_to_int(p->wind_speed_of_gust_ms * 10.0f)
                                    : LV_CHART_POINT_NONE;
    }

    int32_t temp_range_min = round_to_int(temp_min) - 1;
    int32_t temp_range_max = round_to_int(temp_max) + 1;
    if (temp_range_max <= temp_range_min) {
        temp_range_max = temp_range_min + 1;
    }

    /* Precipitation-min/max bar charts: scaled off the max series (>= min
     * always), same "shared range, both charts kept in sync" scheme as the
     * wind/gust pair below. */
    int32_t precip_range_max = round_to_int(precip_max * 10.0f) + 2;
    if (precip_range_max < 10) {
        precip_range_max = 10;
    }

    lv_chart_set_point_count(s_precip_max_chart, fc->point_count);
    lv_chart_set_axis_range(s_precip_max_chart, LV_CHART_AXIS_PRIMARY_Y, 0,
                            precip_range_max * PRECIP_AXIS_COMPRESSION);
    lv_chart_set_series_ext_y_array(s_precip_max_chart, s_precip_max_series, s_precip_max_chart_data);
    lv_chart_refresh(s_precip_max_chart); /* force redraw - see below */

    lv_chart_set_point_count(s_precip_chart, fc->point_count);
    lv_chart_set_axis_range(s_precip_chart, LV_CHART_AXIS_PRIMARY_Y, 0, precip_range_max * PRECIP_AXIS_COMPRESSION);
    lv_chart_set_series_ext_y_array(s_precip_chart, s_precip_series, s_precip_chart_data);
    /* The series shares one static buffer whose contents we overwrite in place;
     * lv_chart_set_point_count() bails out when the length is unchanged and
     * set_series_ext_y_array() doesn't invalidate, so between nowcast refreshes
     * (same point_count) the bars would otherwise never redraw. */
    lv_chart_refresh(s_precip_chart);

    for (int i = 0; i < fc->point_count; i++) {
        float v = fc->points[i].air_temperature_c;
        int32_t x = (fc->point_count > 1) ? (int32_t)i * (CHART_W - 1) / (fc->point_count - 1) : 0;
        int32_t y = (int32_t)((temp_range_max - v) / (temp_range_max - temp_range_min) * (CHART_H - 1));
        s_temp_line_points[i].x = x;
        s_temp_line_points[i].y = y;
        s_temp_line_values[i] = v;
    }
    /* The merged series length varies (nowcast steps + hourly points), and
     * drawing the full YR_FORECAST_MAX_POINTS array would trail a line back
     * through the stale/zero tail entries. */
    s_temp_line_count = fc->point_count > 1 ? (uint32_t)fc->point_count : 0;
    lv_obj_invalidate(s_temp_line);

    place_temp_markers(fc, temp_min_idx, temp_max_idx);
    place_precip_markers(fc, precip_range_max);

    /* Wind/gust bar chart: full m/s per unit, +2 m/s headroom, min 6 m/s so a
     * calm forecast still has a sensible axis. Scaled off whichever of the
     * two is higher - almost always the gust - so a strong gust forecast
     * never clips off the top of its own chart. Both charts share this same
     * range and point count (see s_gust_chart's declaration). */
    float wind_or_gust_max = (gust_max > wind_max) ? gust_max : wind_max;
    int32_t wind_range_max = round_to_int(wind_or_gust_max * 10.0f) + 20;
    if (wind_range_max < 60) {
        wind_range_max = 60;
    }
    lv_chart_set_point_count(s_gust_chart, fc->point_count);
    lv_chart_set_axis_range(s_gust_chart, LV_CHART_AXIS_PRIMARY_Y, 0, wind_range_max);
    lv_chart_set_series_ext_y_array(s_gust_chart, s_gust_series, s_gust_chart_data);
    lv_chart_refresh(s_gust_chart); /* force redraw - see the precip chart above */

    lv_chart_set_point_count(s_wind_chart, fc->point_count);
    lv_chart_set_axis_range(s_wind_chart, LV_CHART_AXIS_PRIMARY_Y, 0, wind_range_max);
    lv_chart_set_series_ext_y_array(s_wind_chart, s_wind_series, s_wind_chart_data);
    lv_chart_refresh(s_wind_chart); /* force redraw - see the precip chart above */

    place_wind_markers(fc);

    char last_label[6] = "";
    for (int i = 0; i < NUM_HOUR_LABELS; i++) {
        int idx = (fc->point_count - 1) * i / (NUM_HOUR_LABELS - 1);

        char hour[6];
        format_hour_label(fc->points[idx].epoch_utc, hour, sizeof(hour));
        /* Adjacent slots can round to the same hour where the near term is
         * compressed - blank the duplicate rather than print it twice. */
        lv_label_set_text(s_hour_labels[i], strcmp(hour, last_label) == 0 ? "" : hour);
        if (strcmp(hour, last_label) != 0) {
            snprintf(last_label, sizeof(last_label), "%s", hour);
        }

        set_weather_icon(s_icon_slots[i], fc->points[idx].symbol_code);

        /* Wind direction arrow: the PNG points north at rotation 0; rotate it
         * to the direction the wind blows TO (from-direction + 180). LVGL
         * rotation is in 0.1-degree units, clockwise. */
        int32_t to_deg = ((int32_t)fc->points[idx].wind_from_deg + 180) % 360;
        lv_image_set_rotation(s_wind_dir_arrows[i], to_deg * 10);
        lv_obj_clear_flag(s_wind_dir_arrows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Linear-interpolate a per-point float field of the hourly forecast at an
 * arbitrary epoch - used to give the finer Nowcast points a temperature and
 * wind speed (the Nowcast itself only carries them for its first step). */
static float interp_base(const yr_forecast_t *base, int64_t epoch,
                         float (*get)(const yr_forecast_point_t *))
{
    if (base->point_count == 0) {
        return 0.0f;
    }
    if (epoch <= base->points[0].epoch_utc) {
        return get(&base->points[0]);
    }
    for (int i = 1; i < base->point_count; i++) {
        int64_t e1 = base->points[i].epoch_utc;
        if (epoch <= e1) {
            int64_t e0 = base->points[i - 1].epoch_utc;
            float v0 = get(&base->points[i - 1]);
            float v1 = get(&base->points[i]);
            if (e1 == e0) {
                return v1;
            }
            return v0 + (float)(epoch - e0) / (float)(e1 - e0) * (v1 - v0);
        }
    }
    return get(&base->points[base->point_count - 1]);
}

/* Nearest hourly-forecast point to an epoch (by time), for Nowcast fields
 * that don't interpolate cleanly - the weather symbol, and wind direction
 * (angles wrap at 360). Returns NULL only if base is empty. */
static const yr_forecast_point_t *nearest_base_point(const yr_forecast_t *base, int64_t epoch)
{
    const yr_forecast_point_t *best = NULL;
    int64_t best_dist = INT64_MAX;
    for (int i = 0; i < base->point_count; i++) {
        int64_t d = base->points[i].epoch_utc - epoch;
        if (d < 0) {
            d = -d;
        }
        if (d < best_dist) {
            best_dist = d;
            best = &base->points[i];
        }
    }
    return best;
}

static bool any_cache_valid(void)
{
    for (int i = 0; i < s_cfg.location_count; i++) {
        if (s_fc_valid[i]) {
            return true;
        }
    }
    return false;
}

/* Whether the overview is still waiting on its first forecast. False (nothing
 * to wait for) when no location shows weather at all - e.g. a radar-only
 * setup - so the overview never gets stuck on "Henter oversikt..." forever;
 * the IP/heap footnotes render regardless via update_overview(). */
static bool overview_loading(void)
{
    return s_weather_count > 0 && !any_cache_valid();
}

static lv_color_t alert_lv_color(met_alert_color_t c)
{
    switch (c) {
    case MET_ALERT_RED:    return lv_palette_main(LV_PALETTE_RED);
    case MET_ALERT_ORANGE: return lv_palette_main(LV_PALETTE_ORANGE);
    default:               return lv_palette_main(LV_PALETTE_YELLOW);
    }
}

/* The most severe of a location's currently active alerts, or NULL if it has
 * none (either nothing active, or its cache isn't valid yet). */
static const met_alert_t *alert_worst(int loc)
{
    if (!s_alert_valid[loc] || s_alert_cache[loc]->count == 0) {
        return NULL;
    }
    const met_alert_t *worst = &s_alert_cache[loc]->alerts[0];
    for (int j = 1; j < s_alert_cache[loc]->count; j++) {
        if (s_alert_cache[loc]->alerts[j].color > worst->color) {
            worst = &s_alert_cache[loc]->alerts[j];
        }
    }
    return worst;
}

/* Refresh the selected location's alert banner on the detail screen (top
 * centre, between the location name and the "updated" timestamp). Must be
 * called under the adapter lock. */
static void update_alert_banner(int loc)
{
    const met_alert_t *worst = alert_worst(loc);
    if (worst == NULL) {
        lv_obj_add_flag(s_alert_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_style_bg_color(s_alert_label, alert_lv_color(worst->color), 0);
    int extra = s_alert_cache[loc]->count - 1;
    if (extra > 0) {
        lv_label_set_text_fmt(s_alert_label, "OBS: %s (+%d)", worst->event_name, extra);
    } else {
        lv_label_set_text_fmt(s_alert_label, "OBS: %s", worst->event_name);
    }
    lv_obj_clear_flag(s_alert_label, LV_OBJ_FLAG_HIDDEN);
}

/* Refresh every row's alert badge in the overview table. Part of
 * update_overview() (below) so every call site that repaints the overview
 * keeps the badges current for free. */
static void update_overview_alerts(void)
{
    for (int row = 0; row < s_weather_count; row++) {
        const met_alert_t *worst = alert_worst(s_wx_loc[row]);
        if (worst == NULL) {
            lv_obj_add_flag(s_ov_alert[row], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_style_bg_color(s_ov_alert[row], alert_lv_color(worst->color), 0);
        lv_obj_clear_flag(s_ov_alert[row], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Repaint the overview table from the per-location caches. Uses the most
 * recent cache's first point as "now" (the device has no wall clock), aligns
 * it to the hour, and for each column samples the nearest hourly point for
 * temperature + weather symbol and sums precipitation over the next OV_STEP_H
 * hours. Missing caches show dashes. Must be called under the adapter lock. */
static void update_overview(void)
{
    update_overview_alerts();

    lv_label_set_text(s_ov_ip_label, wifi_provision_get_ip());
    lv_obj_align(s_ov_ip_label, LV_ALIGN_BOTTOM_RIGHT, -OV_X, -4); /* re-anchor: text width changed */

    lv_label_set_text_fmt(s_ov_heap_label, "%u",
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lv_obj_align(s_ov_heap_label, LV_ALIGN_BOTTOM_LEFT, OV_X, -4); /* re-anchor: text width changed */

    int64_t now_epoch = 0;
    for (int i = 0; i < s_cfg.location_count; i++) {
        if (s_fc_valid[i] && s_fc_cache[i]->point_count > 0) {
            int64_t e = s_fc_cache[i]->points[0].epoch_utc;
            if (e > now_epoch) {
                now_epoch = e;
            }
        }
    }
    if (now_epoch == 0) {
        return;
    }
    int64_t t0 = (now_epoch / 3600) * 3600;

    for (int c = 0; c < OV_COLS; c++) {
        time_t tt = (time_t)(t0 + (int64_t)c * OV_STEP_H * 3600);
        struct tm lt;
        localtime_r(&tt, &lt);
        lv_label_set_text_fmt(s_ov_hdr[c], "kl %02d", lt.tm_hour);
    }

    for (int row = 0; row < s_weather_count; row++) {
        const int i = s_wx_loc[row]; /* location shown on this row */
        lv_label_set_text(s_ov_name[row], s_cfg.locations[i].name);

        const bool ok = s_fc_valid[i] && s_fc_cache[i]->point_count > 0;
        const yr_forecast_t *fc = ok ? s_fc_cache[i] : NULL;

        for (int c = 0; c < OV_COLS; c++) {
            lv_obj_t *icon = s_ov_icon[row][c];
            lv_obj_t *cell = s_ov_cell[row][c];

            if (!ok) {
                lv_label_set_text(cell, "\xE2\x80\x93"); /* en dash */
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                continue;
            }

            int64_t block_start = t0 + (int64_t)c * OV_STEP_H * 3600;
            const yr_forecast_point_t *np = nearest_base_point(fc, block_start);

            float psum = 0.0f;
            for (int k = 0; k < fc->point_count; k++) {
                int64_t e = fc->points[k].epoch_utc;
                if (e >= block_start && e < block_start + OV_STEP_H * 3600) {
                    psum += fc->points[k].precipitation_mm;
                }
            }

            if (psum >= 0.05f) {
                lv_label_set_text_fmt(cell, "%.0f\xC2\xB0\n%.1f mm",
                                      (double)np->air_temperature_c, (double)psum);
            } else {
                lv_label_set_text_fmt(cell, "%.0f\xC2\xB0", (double)np->air_temperature_c);
            }
            set_weather_icon(icon, np->symbol_code);
        }
    }
}

/* Build the rendered series: the Nowcast's near-term steps (10-min spacing,
 * radar precipitation as mm/h - directly comparable to the hourly amounts),
 * followed by the hourly forecast points that start after the Nowcast window.
 * `dst` and `base` must be different buffers; `nc` is assumed valid with
 * radar coverage. */
static void merge_nowcast(yr_forecast_t *dst, const yr_forecast_t *base, const yr_nowcast_t *nc)
{
    *dst = *base;

    int64_t last_nc_epoch = base->points[0].epoch_utc;
    int m = 0;

    for (int i = 0; i < nc->point_count && m < YR_FORECAST_MAX_POINTS; i += NOWCAST_MERGE_STRIDE) {
        const yr_nowcast_point_t *s = &nc->points[i];
        yr_forecast_point_t p = { 0 };

        snprintf(p.hour_minute, sizeof(p.hour_minute), "%s", s->hour_minute);
        p.is_first_of_day = s->is_first_of_day;
        p.epoch_utc = s->epoch_utc;
        p.precipitation_mm = s->precipitation_rate;
        /* The nowcast has no uncertainty range (it's radar-derived, not an
         * ensemble forecast) - no bar behind this one worth drawing. */
        p.precipitation_min_mm = s->precipitation_rate;
        p.precipitation_max_mm = s->precipitation_rate;

        const yr_forecast_point_t *nb = nearest_base_point(base, s->epoch_utc);
        if (s->has_instant_details) {
            p.air_temperature_c = s->air_temperature_c;
            p.wind_speed_ms = s->wind_speed_ms;
            p.wind_speed_of_gust_ms = s->wind_speed_of_gust_ms;
            p.wind_from_deg = s->wind_from_deg;
        } else {
            p.air_temperature_c = interp_base(base, s->epoch_utc, pt_temp);
            p.wind_speed_ms = interp_base(base, s->epoch_utc, pt_wind);
            p.wind_speed_of_gust_ms = interp_base(base, s->epoch_utc, pt_wind_gust);
            p.wind_from_deg = nb ? nb->wind_from_deg : 0.0f;
        }

        const char *sym = s->symbol_code[0] ? s->symbol_code
                                            : (nb ? nb->symbol_code : "");
        snprintf(p.symbol_code, sizeof(p.symbol_code), "%s", sym);

        dst->points[m++] = p;
        last_nc_epoch = s->epoch_utc;
    }

    for (int i = 0; i < base->point_count && m < YR_FORECAST_MAX_POINTS; i++) {
        if (base->points[i].epoch_utc <= last_nc_epoch) {
            continue; /* this hour is inside the nowcast window */
        }
        dst->points[m++] = base->points[i];
    }

    dst->point_count = m;

    /* The nowcast is the fresher data - show its issue time. */
    if (nc->updated_hour_minute[0]) {
        snprintf(dst->updated_hour_minute, sizeof(dst->updated_hour_minute), "%s",
                 nc->updated_hour_minute);
    }
}

/* Every chart and label in update_ui_with_forecast positions itself by array
 * INDEX (x = i * width / (point_count - 1); the bottom hour labels and the
 * precip/wind lv_chart bars all do the same, and lv_chart's own bar layout
 * can't be told to do otherwise). That's only proportional to elapsed time
 * if the points themselves are evenly time-spaced - true of a plain hourly
 * forecast, but not of a nowcast-merged series, which packs many 5/10-minute
 * steps into the first ~2 hours followed by sparse hourly ones: an hour of
 * near-term nowcast then occupies as many index-slots (and so as much of the
 * x-axis) as several hours further out.
 *
 * Fix: re-sample `src` onto `dst`, the same point count but evenly spaced in
 * TIME from its first to its last point, before anything renders it. This
 * makes uniform index-spacing correct again for every consumer. Continuous
 * fields (temperature, wind speed) are linearly interpolated between the
 * bracketing source points (interp_base); everything else (precipitation,
 * wind direction, symbol) is taken from the nearest source point in time -
 * the same approximations merge_nowcast already makes for the same reason. */
static void resample_uniform_time(yr_forecast_t *dst, const yr_forecast_t *src)
{
    int n = src->point_count;
    if (n < 2) {
        *dst = *src;
        return;
    }
    int64_t t0 = src->points[0].epoch_utc;
    int64_t t1 = src->points[n - 1].epoch_utc;
    if (t1 <= t0) {
        *dst = *src;
        return;
    }

    yr_forecast_t out = *src; /* carries over valid / updated_hour_minute / etc. */
    for (int i = 0; i < n; i++) {
        int64_t target = t0 + (int64_t)i * (t1 - t0) / (n - 1);
        const yr_forecast_point_t *near = nearest_base_point(src, target);

        yr_forecast_point_t p = *near;
        p.epoch_utc = target;
        p.air_temperature_c = interp_base(src, target, pt_temp);
        p.wind_speed_ms = interp_base(src, target, pt_wind);
        p.wind_speed_of_gust_ms = interp_base(src, target, pt_wind_gust);
        out.points[i] = p;
    }
    out.point_count = n;
    *dst = out;
}

/* Shown on the status label while wifi_provision works (connecting, or the
 * setup-portal instructions). */
static void provision_status_cb(const char *msg)
{
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_label_set_text(s_status_label, msg);
        esp_lv_adapter_unlock();
    }
}

/* One ADS-B poll for the radar of location `loc`, shown only if the screen is
 * still `for_view` when the reply lands. The previous plot is kept on a failed
 * poll (it just keeps dead-reckoning) unless there is none yet. */
static void radar_poll(int loc, adsb_result_t *scratch, int for_view)
{
    double lat = atof(s_cfg.locations[loc].lat);
    double lon = atof(s_cfg.locations[loc].lon);
    bool ok = (adsb_client_fetch(lat, lon, (float)s_cfg.radar_km[loc], scratch) == ESP_OK);

    if (s_view_index != for_view || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (ok) {
        lv_label_set_text(s_status_label, "");
        radar_apply(scratch);
    } else if (!s_radar_valid) {
        lv_label_set_text(s_status_label, "Kunne ikke hente fly. Pr\xC3\xB8ver igjen...");
    }
    esp_lv_adapter_unlock();
}

/* --------------------------------------------------------------------------
 * Coastline (components/coast.bin, built by scripts/build_coast.py from
 * OpenStreetMap, in the "coast" partition): a grid of tiles, each a list of
 * lines stored as varint steps from point to point (layout in the script). coast_render reads the
 * tiles around a location and draws them, anti-aliased, into s_coast_px once
 * per location and range; the radar draw callback then only blits that image.
 * ------------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t rows, cols;
    int32_t lat_min_e5, lon_min_e5, tile_dlat_e5, tile_dlon_e5, unit_e5;
} coast_hdr_t;

/* One varint (7 bits per byte, low first); false if it runs past `end`. */
static bool coast_varint(const uint8_t **p, const uint8_t *end, uint32_t *v)
{
    uint32_t r = 0;
    for (int shift = 0; *p < end && shift < 32; shift += 7) {
        uint8_t b = *(*p)++;
        r |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *v = r;
            return true;
        }
    }
    return false;
}

static void coast_plot(int x, int y, float cover)
{
    if (x < 0 || y < 0 || x >= COAST_D || y >= COAST_D) {
        return;
    }
    int dx = x - RADAR_R, dy = y - RADAR_R;
    if (dx * dx + dy * dy > RADAR_R * RADAR_R) {
        return;
    }
    int a = (int)(cover * 255.0f + 0.5f);
    uint8_t *p = &s_coast_px[y * COAST_D + x];
    if (a > *p) {
        *p = (uint8_t)a;
    }
}

/* Xiaolin Wu's anti-aliased line, in image pixels. */
static void coast_line(float x0, float y0, float x1, float y1)
{
    if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) ||
        (x0 >= COAST_D && x1 >= COAST_D) || (y0 >= COAST_D && y1 >= COAST_D)) {
        return;
    }
    bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) {
        float t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        float t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    float dx = x1 - x0;
    float grad = dx > 0.0f ? (y1 - y0) / dx : 1.0f;
    int xs = (int)lroundf(x0), xe = (int)lroundf(x1);
    float y = y0 + grad * ((float)xs - x0);
    for (int x = xs; x <= xe; x++, y += grad) {
        int yi = (int)floorf(y);
        float f = y - (float)yi;
        if (steep) {
            coast_plot(yi, x, 1.0f - f);
            coast_plot(yi + 1, x, f);
        } else {
            coast_plot(x, yi, 1.0f - f);
            coast_plot(x, yi + 1, f);
        }
    }
}

/* Water mask labels while coast_render works on s_water_px; afterwards it
 * holds 255 for water and 0 for everything else. */
enum { WATER_UNKNOWN, WATER_SEA, WATER_LAND, WATER_COAST, WATER_COAST_SEA_TMP, WATER_DONE = 0x80, WATER_DONE_SEA = 0xC0 };

static bool coast_in_disc(int x, int y)
{
    int dx = x - RADAR_R, dy = y - RADAR_R;
    return x >= 0 && y >= 0 && x < COAST_D && y < COAST_D && dx * dx + dy * dy <= RADAR_R * RADAR_R;
}

/* In coast.bin the coastlines run with the water on the left and the land on
 * the right (the reverse of OpenStreetMap's own ways, as checked on screen
 * against the map). coast_seed notes the middle of each coastline segment and the normal
 * pointing to its water side; coast_fill_water uses them once the whole
 * coastline is drawn. */
static void coast_seed(float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float mx = (x0 + x1) / 2.0f, my = (y0 + y1) / 2.0f;
    if (len < 1.0f || s_water_n_seeds >= WATER_SEEDS_MAX ||
        !coast_in_disc((int)lroundf(mx), (int)lroundf(my))) {
        return;
    }
    /* On screen (y down), the left of direction (dx, dy) is (dy, -dx). */
    s_water_seeds[s_water_n_seeds++] = (water_seed_t){ mx, my, dy / len, -dx / len };
}

/* From a segment's middle, step along `dir` off the line and mark the first
 * pixel clear of it as `v`, unless another line comes first. */
static void water_mark_side(const water_seed_t *sd, float dir, uint8_t v)
{
    for (float t = 0.5f; t <= 4.0f; t += 0.5f) {
        int x = (int)lroundf(sd->x + dir * sd->nx * t), y = (int)lroundf(sd->y + dir * sd->ny * t);
        if (!coast_in_disc(x, y)) {
            return;
        }
        uint8_t *w = &s_water_px[y * COAST_D + x];
        if (*w != WATER_COAST) {
            if (*w == WATER_UNKNOWN) {
                *w = v;
            }
            return;
        }
    }
}

/* Split the disc into the areas the coastline in s_coast_px separates, and
 * make each area sea or land by a vote of the seed pixels inside it (a few
 * seeds land on the wrong side where the coast bends tightly). An area with no
 * seeds (no coast in view) stays unfilled; coastline pixels take the side most
 * of their neighbours are on. */
static void coast_fill_water(void)
{
    const int n = COAST_D * COAST_D;
    uint32_t *q = heap_caps_malloc(n * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (q == NULL) {
        memset(s_water_px, 0, n);
        return;
    }
    for (int i = 0; i < n; i++) {
        s_water_px[i] = s_coast_px[i] > 0 ? WATER_COAST : WATER_UNKNOWN;
    }
    for (int i = 0; i < s_water_n_seeds; i++) {
        water_mark_side(&s_water_seeds[i], 1.0f, WATER_SEA);
        water_mark_side(&s_water_seeds[i], -1.0f, WATER_LAND);
    }
    static const int8_t nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int start = 0; start < n; start++) {
        uint8_t v0 = s_water_px[start];
        if (v0 == WATER_COAST || (v0 & WATER_DONE) || !coast_in_disc(start % COAST_D, start / COAST_D)) {
            continue;
        }
        /* Flood the area, marking it done as it goes; q holds its pixels. */
        int head = 0, tail = 0, sea = 0, land = 0;
        q[tail++] = start;
        s_water_px[start] |= WATER_DONE;
        while (head < tail) {
            int i = q[head++];
            uint8_t v = s_water_px[i] & ~WATER_DONE;
            sea += (v == WATER_SEA);
            land += (v == WATER_LAND);
            int x = i % COAST_D, y = i / COAST_D;
            for (int k = 0; k < 4; k++) {
                int nx = x + nb[k][0], ny = y + nb[k][1];
                if (!coast_in_disc(nx, ny)) {
                    continue;
                }
                uint8_t *w = &s_water_px[ny * COAST_D + nx];
                if (*w != WATER_COAST && !(*w & WATER_DONE)) {
                    *w |= WATER_DONE;
                    q[tail++] = ny * COAST_D + nx;
                }
            }
        }
        const uint8_t fill = (sea > land) ? WATER_DONE_SEA : WATER_DONE;
        for (int i = 0; i < tail; i++) {
            s_water_px[q[i]] = fill;
        }
    }
    free(q);
    for (int i = 0; i < n; i++) {
        if (s_water_px[i] != WATER_COAST) {
            continue;
        }
        int x = i % COAST_D, y = i / COAST_D, sea = 0, land = 0;
        for (int k = 0; k < 4; k++) {
            int nx = x + nb[k][0], ny = y + nb[k][1];
            if (coast_in_disc(nx, ny)) {
                uint8_t w = s_water_px[ny * COAST_D + nx];
                sea += (w == WATER_DONE_SEA);
                land += (w == WATER_DONE);
            }
        }
        s_water_px[i] = (sea > land) ? WATER_COAST_SEA_TMP : WATER_COAST;
    }
    for (int i = 0; i < n; i++) {
        uint8_t v = s_water_px[i];
        s_water_px[i] = (v == WATER_DONE_SEA || v == WATER_COAST_SEA_TMP) ? 255 : 0;
    }
}

/* Draw location `loc`'s coastline at `range` km into s_coast_px, unless it's
 * already there. Runs in the weather task; flash reads, so not under the
 * adapter lock except to flip s_coast_valid. */
static void coast_render(int loc, int range)
{
    static const esp_partition_t *part;
    static coast_hdr_t hdr;
    if (s_coast_px == NULL || (s_coast_loc == loc && s_coast_km == range && s_coast_valid)) {
        return;
    }
    if (part == NULL) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "coast");
        if (part == NULL || esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
            memcmp(hdr.magic, "CST2", 4) != 0) {
            ESP_LOGW(TAG, "No coastline data in the coast partition");
            s_coast_px = NULL; /* don't try again */
            return;
        }
    }

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        s_coast_valid = false;
        esp_lv_adapter_unlock();
    }
    memset(s_coast_px, 0, COAST_D * COAST_D);
    s_water_n_seeds = 0;

    const double lat0 = atof(s_cfg.locations[loc].lat);
    const double lon0 = atof(s_cfg.locations[loc].lon);
    const float km_lat = 110.574f / 1e5f;                                   /* km per 1e-5 deg */
    const float km_lon = 111.320f * cosf((float)(lat0 * M_PI / 180.0)) / 1e5f;
    const float px_per_km = (float)RADAR_R / (float)range;
    const double span_lat = range / 110.574, span_lon = range / (111.320 * cos(lat0 * M_PI / 180.0));

    /* One tile of margin: a line's last point can reach into the next tile. */
    int r0 = (int)floor(((lat0 - span_lat) * 1e5 - hdr.lat_min_e5) / hdr.tile_dlat_e5) - 1;
    int r1 = (int)floor(((lat0 + span_lat) * 1e5 - hdr.lat_min_e5) / hdr.tile_dlat_e5) + 1;
    int c0 = (int)floor(((lon0 - span_lon) * 1e5 - hdr.lon_min_e5) / hdr.tile_dlon_e5) - 1;
    int c1 = (int)floor(((lon0 + span_lon) * 1e5 - hdr.lon_min_e5) / hdr.tile_dlon_e5) + 1;
    r0 = r0 < 0 ? 0 : r0;
    c0 = c0 < 0 ? 0 : c0;
    r1 = r1 >= hdr.rows ? hdr.rows - 1 : r1;
    c1 = c1 >= hdr.cols ? hdr.cols - 1 : c1;

    const size_t index_off = sizeof(hdr);
    const size_t data_off = index_off + 4 * ((size_t)hdr.rows * hdr.cols + 1);
    uint32_t idx[64];
    uint8_t *buf = NULL;
    size_t buf_cap = 0;
    int32_t *pts = NULL; /* one decoded line: x, y pairs */
    uint32_t pts_cap = 0;
    int lines = 0;

    for (int r = r0; r <= r1 && c1 >= c0 && c1 - c0 + 2 <= 64; r++) {
        int n_idx = c1 - c0 + 2;
        if (esp_partition_read(part, index_off + 4 * ((size_t)r * hdr.cols + c0), idx, 4 * n_idx) != ESP_OK) {
            break;
        }
        size_t len = idx[n_idx - 1] - idx[0];
        if (len == 0) {
            continue;
        }
        if (len > buf_cap) {
            uint8_t *nb = heap_caps_realloc(buf, len, MALLOC_CAP_SPIRAM);
            if (nb == NULL) {
                break;
            }
            buf = nb;
            buf_cap = len;
        }
        if (esp_partition_read(part, data_off + idx[0], buf, len) != ESP_OK) {
            break;
        }
        for (int c = c0; c <= c1; c++) {
            const uint8_t *p = buf + (idx[c - c0] - idx[0]);
            const uint8_t *end = buf + (idx[c - c0 + 1] - idx[0]);
            /* Tile corner relative to the centre, in 1e-5 degrees. */
            float tile_dlat = (float)(hdr.lat_min_e5 + (int64_t)r * hdr.tile_dlat_e5 - lat0 * 1e5);
            float tile_dlon = (float)(hdr.lon_min_e5 + (int64_t)c * hdr.tile_dlon_e5 - lon0 * 1e5);
            int32_t cur[2] = { 0, 0 }; /* e5 from the tile corner; see coast.bin's layout */
            uint32_t n;
            while (p < end && coast_varint(&p, end, &n) && n > 0) {
                if (n > pts_cap) {
                    int32_t *np = heap_caps_realloc(pts, 2 * sizeof(int32_t) * n, MALLOC_CAP_SPIRAM);
                    if (np == NULL) {
                        break;
                    }
                    pts = np;
                    pts_cap = n;
                }
                int32_t lo[2] = { INT32_MAX, INT32_MAX }, hi[2] = { INT32_MIN, INT32_MIN };
                uint32_t i = 0;
                bool ok = true;
                for (; ok && i < n; i++) {
                    for (int k = 0; k < 2; k++) {
                        uint32_t z;
                        if (!coast_varint(&p, end, &z)) {
                            ok = false;
                            break;
                        }
                        cur[k] += (int32_t)((z >> 1) ^ -(z & 1)) * hdr.unit_e5;
                        pts[2 * i + k] = cur[k];
                        lo[k] = cur[k] < lo[k] ? cur[k] : lo[k];
                        hi[k] = cur[k] > hi[k] ? cur[k] : hi[k];
                    }
                }
                if (!ok) {
                    break; /* truncated tile */
                }
                /* An island a couple of pixels across at this scale is only
                 * speckle: skip closed rings that small. */
                if (n >= 3 && pts[0] == pts[2 * (n - 1)] && pts[1] == pts[2 * (n - 1) + 1] &&
                    (hi[0] - lo[0]) * km_lon * px_per_km < 2.0f &&
                    (hi[1] - lo[1]) * km_lat * px_per_km < 2.0f) {
                    continue;
                }
                float px = 0, py = 0;
                for (i = 0; i < n; i++) {
                    float x = RADAR_R + (tile_dlon + pts[2 * i]) * km_lon * px_per_km;
                    float y = RADAR_R - (tile_dlat + pts[2 * i + 1]) * km_lat * px_per_km;
                    if (i > 0) {
                        coast_line(px, py, x, y);
                        coast_seed(px, py, x, y);
                    }
                    px = x;
                    py = y;
                }
                lines++;
            }
        }
    }
    free(buf);
    free(pts);
    coast_fill_water();
    ESP_LOGI(TAG, "Coastline for %s: %d line(s) in tiles %d-%d x %d-%d",
             s_cfg.locations[loc].name, lines, r0, r1, c0, c1);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        s_coast_loc = loc;
        s_coast_km = range;
        /* Unless the screen moved on while drawing. */
        s_coast_valid = (s_radar_loc == loc && s_radar_range_km == range);
        lv_obj_invalidate(s_radar_canvas);
        esp_lv_adapter_unlock();
    }
}

static void ships_poll(int loc, ais_result_t *scratch, int for_view)
{
    double lat = atof(s_cfg.locations[loc].lat);
    double lon = atof(s_cfg.locations[loc].lon);
    esp_err_t err = ais_client_fetch(s_cfg.ais_client_id, s_cfg.ais_client_secret,
                                     lat, lon, (float)s_cfg.ship_km[loc], s_cfg.ship_min_len_m[loc],
                                     scratch);

    if (s_view_index != for_view || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (err == ESP_OK) {
        lv_label_set_text(s_status_label, "");
        ships_apply(scratch);
    } else if (err == ESP_ERR_INVALID_ARG) {
        lv_label_set_text(s_status_label, "Mangler BarentsWatch-n\xC3\xB8kkel");
    } else if (err == ESP_ERR_INVALID_STATE) {
        lv_label_set_text(s_status_label, "Innlogging feilet");
    } else if (!s_radar_valid) {
        lv_label_set_text(s_status_label, "Kunne ikke hente skip. Pr\xC3\xB8ver igjen...");
    }
    esp_lv_adapter_unlock();
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

/* --------------------------------------------------------------------------
 * Screenshot: GET /screen.png on the config web server returns the current
 * screen as a PNG. The image data is stored uncompressed (deflate "stored"
 * blocks, one per row), which needs no compressor and streams row by row;
 * about 1.1 MB for 800 x 480.
 * ------------------------------------------------------------------------ */

typedef struct {
    httpd_req_t *req;
    uint8_t buf[2048];
    size_t n;
    uint32_t crc;   /* of the PNG chunk being written */
    esp_err_t err;
} png_out_t;

static void png_put(png_out_t *o, const void *data, size_t len)
{
    const uint8_t *p = data;
    o->crc = esp_rom_crc32_le(o->crc, p, len);
    while (len > 0 && o->err == ESP_OK) {
        size_t k = sizeof(o->buf) - o->n;
        k = k < len ? k : len;
        memcpy(o->buf + o->n, p, k);
        o->n += k;
        p += k;
        len -= k;
        if (o->n == sizeof(o->buf)) {
            o->err = httpd_resp_send_chunk(o->req, (const char *)o->buf, o->n);
            o->n = 0;
        }
    }
}

static void png_put_u32(png_out_t *o, uint32_t v)
{
    uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };
    png_put(o, b, 4);
}

static void png_chunk_start(png_out_t *o, const char *type, uint32_t len)
{
    png_put_u32(o, len);
    o->crc = 0;
    png_put(o, type, 4);
}

static void png_chunk_end(png_out_t *o)
{
    png_put_u32(o, o->crc);
}

static esp_err_t h_screenshot(httpd_req_t *req)
{
    lv_draw_buf_t *snap = NULL;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
        esp_lv_adapter_unlock();
    }
    png_out_t *o = heap_caps_calloc(1, sizeof(*o), MALLOC_CAP_SPIRAM);
    uint8_t *row = heap_caps_malloc(1 + 3 * (snap ? snap->header.w : 0), MALLOC_CAP_SPIRAM);
    if (snap == NULL || o == NULL || row == NULL) {
        if (snap != NULL) {
            lv_draw_buf_destroy(snap);
        }
        free(o);
        free(row);
        return httpd_resp_send_500(req);
    }
    const uint32_t w = snap->header.w, h = snap->header.h;
    const uint32_t row_len = 1 + 3 * w; /* filter byte + RGB */
    o->req = req;
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"screen.png\"");

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    png_put(o, sig, sizeof(sig));
    png_chunk_start(o, "IHDR", 13);
    png_put_u32(o, w);
    png_put_u32(o, h);
    static const uint8_t ihdr[5] = { 8, 2, 0, 0, 0 }; /* 8-bit RGB, no interlace */
    png_put(o, ihdr, sizeof(ihdr));
    png_chunk_end(o);

    png_chunk_start(o, "IDAT", 2 + h * (5 + row_len) + 4);
    static const uint8_t zhdr[2] = { 0x78, 0x01 };
    png_put(o, zhdr, sizeof(zhdr));
    uint32_t a1 = 1, a2 = 0; /* Adler-32 of the raw rows */
    for (uint32_t y = 0; y < h && o->err == ESP_OK; y++) {
        const uint8_t *src = snap->data + y * snap->header.stride;
        row[0] = 0; /* no filter */
        for (uint32_t x = 0; x < w; x++) { /* LVGL's RGB888 is B, G, R in memory */
            row[1 + 3 * x] = src[3 * x + 2];
            row[2 + 3 * x] = src[3 * x + 1];
            row[3 + 3 * x] = src[3 * x];
        }
        for (uint32_t i = 0; i < row_len; i++) {
            a1 = (a1 + row[i]) % 65521;
            a2 = (a2 + a1) % 65521;
        }
        uint8_t bh[5] = { y == h - 1, row_len & 0xFF, row_len >> 8, ~row_len & 0xFF, (~row_len >> 8) & 0xFF };
        png_put(o, bh, sizeof(bh));
        png_put(o, row, row_len);
    }
    png_put_u32(o, (a2 << 16) | a1);
    png_chunk_end(o);
    png_chunk_start(o, "IEND", 0);
    png_chunk_end(o);

    lv_draw_buf_destroy(snap);
    free(row);
    esp_err_t err = o->err;
    if (err == ESP_OK && o->n > 0) {
        err = httpd_resp_send_chunk(req, (const char *)o->buf, o->n);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    free(o);
    return err;
}

static void yr_weather_task(void *arg)
{
    /* Connects in station mode, or blocks forever in the setup portal (and
     * reboots when the form is saved). */
    wifi_provision_connect(&s_cfg, provision_status_cb);
    wifi_provision_add_get_handler("/screen.png", h_screenshot);

    /* Let WiFi's own connection-setup buffers settle before hitting it with
     * a large TLS handshake - the two compete hard for the same scarce
     * internal DRAM in the first moment after association. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Sync the wall clock purely so the nightly reboot below can be scheduled
     * by real time-of-day. Fire-and-forget: esp_netif_sntp_init() just starts
     * lwip's SNTP client in the background (wait_for_sync=false), and the
     * loop further down treats a still-unsynced clock (reading near the 1970
     * epoch) as "not yet known" and skips scheduling until it catches up. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.wait_for_sync = false;
    esp_netif_sntp_init(&sntp_cfg);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_label_set_text(s_status_label, "Henter v\xC3\xA6rvarsel...");
        esp_lv_adapter_unlock();
    }

    /* All in PSRAM - large, and no reason to compete with mbedtls/TLS for
     * scarce internal DRAM. `scratch` receives each fetch (yr_client zeroes
     * its output, so fetching straight into a cache would wipe the last good
     * copy on a network hiccup); `merged` holds the nowcast-spliced series;
     * `resampled` is what the detail view actually renders (see
     * resample_uniform_time); s_fc_cache[i] keeps each location's last good
     * hourly forecast for the overview. `alert_scratch`/s_alert_cache[i] are
     * the same scratch-then-copy scheme for the severe weather alerts. */
    yr_forecast_t *scratch = heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_SPIRAM);
    yr_forecast_t *merged = heap_caps_malloc(sizeof(*merged), MALLOC_CAP_SPIRAM);
    yr_forecast_t *resampled = heap_caps_malloc(sizeof(*resampled), MALLOC_CAP_SPIRAM);
    yr_nowcast_t *nowcast = heap_caps_malloc(sizeof(*nowcast), MALLOC_CAP_SPIRAM);
    adsb_result_t *adsb_scratch = heap_caps_malloc(sizeof(*adsb_scratch), MALLOC_CAP_SPIRAM);
    ais_result_t *ais_scratch = heap_caps_malloc(sizeof(*ais_scratch), MALLOC_CAP_SPIRAM);
    met_alerts_t *alert_scratch = heap_caps_malloc(sizeof(*alert_scratch), MALLOC_CAP_SPIRAM);
    bool caches_ok = (scratch != NULL && merged != NULL && resampled != NULL &&
                      nowcast != NULL && adsb_scratch != NULL && ais_scratch != NULL &&
                      alert_scratch != NULL);
    for (int i = 0; i < s_cfg.location_count; i++) {
        s_fc_cache[i] = heap_caps_malloc(sizeof(yr_forecast_t), MALLOC_CAP_SPIRAM);
        s_alert_cache[i] = heap_caps_malloc(sizeof(met_alerts_t), MALLOC_CAP_SPIRAM);
        if (s_fc_cache[i] == NULL || s_alert_cache[i] == NULL) {
            caches_ok = false;
        }
    }
    if (!caches_ok) {
        ESP_LOGE(TAG, "Out of memory allocating forecast buffers");
        vTaskDelete(NULL);
        return;
    }

    int active_view = -1;   /* -1 forces a first render; else == s_view_index  */
    bool overview = false;
    bool radar = false;
    bool ships = false;
    int sel = 0;            /* selected location index when not on the overview */
    time_t next_nightly_reboot = 0; /* 0 = not yet scheduled (clock not synced) */
    bool night_dim_active = false;  /* mirrors s_tap_layer's current bg_opa */

    while (1) {
        /* Once a day, purely for memory-pressure hygiene. Checked every loop
         * wake (every few minutes at idle, immediately on a tap) rather than
         * slept for separately - a few minutes of drift past the target hour
         * doesn't matter for housekeeping. */
        time_t now_wall = time(NULL);
        if (now_wall > PLAUSIBLE_EPOCH_S) {
            if (next_nightly_reboot == 0) {
                next_nightly_reboot = compute_next_nightly_reboot(now_wall);
                struct tm lt;
                localtime_r(&next_nightly_reboot, &lt);
                ESP_LOGI(TAG, "Nightly reboot scheduled for %04d-%02d-%02d %02d:%02d local",
                         lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
            } else if (now_wall >= next_nightly_reboot) {
                ESP_LOGW(TAG, "Nightly maintenance reboot (%02d:00 local)", NIGHTLY_REBOOT_HOUR);
                esp_restart();
            }

            /* Night dimming - see s_tap_layer / NIGHT_DIM_*. Also checked
             * every wake; a few minutes of drift at the 22:00/07:00 edges is
             * unnoticeable. */
            struct tm now_lt;
            localtime_r(&now_wall, &now_lt);
            bool want_dim = (now_lt.tm_hour >= NIGHT_DIM_START_HOUR ||
                             now_lt.tm_hour < NIGHT_DIM_END_HOUR);
            if (want_dim != night_dim_active) {
                night_dim_active = want_dim;
                if (esp_lv_adapter_lock(-1) == ESP_OK) {
                    if (want_dim) {
                        lv_obj_set_style_bg_color(s_tap_layer, lv_color_black(), 0);
                        lv_obj_set_style_bg_opa(s_tap_layer, NIGHT_DIM_OPA, 0);
                    } else {
                        lv_obj_set_style_bg_opa(s_tap_layer, LV_OPA_TRANSP, 0);
                    }
                    esp_lv_adapter_unlock();
                }
                ESP_LOGI(TAG, "Night dimming %s (local hour %d)",
                         want_dim ? "on" : "off", now_lt.tm_hour);
            }
        }

        /* Adopt a view switch from the touch handler. Force-refetch the newly
         * selected location's forecast below even if its cache isn't
         * calendar-stale yet: the detail view should only ever show a fully
         * fresh forecast+nowcast pair for the location just switched to,
         * never a stale forecast alone or a stale-forecast/fresh-nowcast mix. */
        int want_view = s_view_index;
        bool force_sel_refetch = false;
        if (want_view != active_view) {
            active_view = want_view;
            const view_stop_t *stop = &s_stops[want_view];
            overview = (stop->kind == STOP_OVERVIEW);
            radar = (stop->kind == STOP_RADAR);
            ships = (stop->kind == STOP_SHIPS);
            sel = overview ? 0 : stop->loc;
            force_sel_refetch = (stop->kind == STOP_WEATHER);

            /* So a reboot of any kind - nightly, power cycle, crash - comes
             * back showing this same screen instead of the overview. */
            app_config_save_last_view((uint8_t)want_view);

            /* The radar keeps its HTTPS connection open between polls; drop it
             * (and its TLS buffers) as soon as the radar isn't on screen. */
            if (!radar) {
                adsb_client_close();
            }
            if (!ships) {
                ais_client_close();
            }

            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                show_view((stop_kind_t)stop->kind);
                if (overview) {
                    /* Always render: the IP/heap footnotes must show up right
                     * away, not only once a forecast lands. */
                    update_overview();
                    lv_label_set_text(s_status_label,
                                      overview_loading() ? "Henter oversikt..." : "");
                } else if (radar) {
                    /* Never show another location's aircraft: blank until the
                     * first fetch for this one lands. */
                    s_radar_valid = false;
                    radar_set_location(sel);
                    lv_label_set_text(s_radar_info, "");
                    lv_obj_invalidate(s_radar_canvas);
                    lv_label_set_text(s_status_label, "Henter fly...");
                } else if (ships) {
                    s_radar_valid = false;
                    ships_set_location(sel);
                    lv_label_set_text(s_radar_info, "");
                    lv_obj_invalidate(s_radar_canvas);
                    lv_label_set_text(s_status_label, "Henter skip...");
                } else {
                    const app_location_t *loc = &s_cfg.locations[sel];
                    lv_label_set_text(s_location_label, loc->name);
                    /* Unlike the forecast, the alert cache carries over as-is
                     * from whatever this location's last fetch found. */
                    update_alert_banner(sel);
                    /* Never render from cache here, even if valid - wait for
                     * the fresh forecast+nowcast fetch below so the graph
                     * doesn't flash an old forecast before the nowcast lands. */
                    lv_label_set_text(s_status_label, "Henter v\xC3\xA6rvarsel...");
                }
                esp_lv_adapter_unlock();
            }
        }

        if (radar) {
            coast_render(sel, s_cfg.radar_km[sel]);
            radar_poll(sel, adsb_scratch, active_view);
        } else if (ships) {
            coast_render(sel, s_cfg.ship_km[sel]);
            ships_poll(sel, ais_scratch, active_view);
        }

        TickType_t now_tk = xTaskGetTickCount(); /* unsigned - wrap-safe deltas */

        /* Refresh any stale or missing location forecast. The selected one is
         * done first so the detail view updates promptly; the rest keep the
         * overview warm. N <= 5 and the cadence is 10 min, so this is a fetch
         * or two per wake at most. Skipped entirely on the radar: it polls
         * every few seconds and is the only thing that should use the network
         * there. Whatever went stale is refreshed when a weather screen or the
         * overview is next shown. */
        for (int k = 0; !radar && !ships && k < s_cfg.location_count; k++) {
            if (s_view_index != active_view) {
                break; /* view changed mid-scan - restart the loop */
            }
            int i = (sel + k) % s_cfg.location_count;
            if (!(s_cfg.show[i] & APP_SHOW_WEATHER)) {
                continue; /* radar-only location: no forecast needed */
            }
            bool stale = !s_fc_valid[i] || (i == sel && force_sel_refetch) ||
                         (now_tk - s_fc_tk[i]) >= pdMS_TO_TICKS(WEATHER_REFRESH_INTERVAL_MS);
            bool alert_stale = !s_alert_valid[i] ||
                                (now_tk - s_alert_tk[i]) >= pdMS_TO_TICKS(ALERT_REFRESH_INTERVAL_MS);
            if (!stale && !alert_stale) {
                continue;
            }

            double lat = atof(s_cfg.locations[i].lat);
            double lon = atof(s_cfg.locations[i].lon);

            if (stale) {
                if (yr_client_fetch_forecast(lat, lon, scratch) == ESP_OK &&
                    scratch->valid && scratch->point_count > 0) {
                    *s_fc_cache[i] = *scratch;
                    s_fc_valid[i] = true;
                    s_fc_tk[i] = now_tk;
                    ESP_LOGI(TAG, "Forecast[%d] %s: %d pts kl. %s (free int %u)",
                             i, s_cfg.locations[i].name, s_fc_cache[i]->point_count,
                             s_fc_cache[i]->updated_hour_minute,
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                } else {
                    ESP_LOGW(TAG, "Forecast[%d] %s failed; keeping previous",
                             i, s_cfg.locations[i].name);
                }
            }

            if (alert_stale) {
                if (met_alerts_client_fetch(lat, lon, alert_scratch) == ESP_OK && alert_scratch->valid) {
                    *s_alert_cache[i] = *alert_scratch;
                    s_alert_valid[i] = true;
                    s_alert_tk[i] = now_tk;
                    if (alert_scratch->count > 0) {
                        ESP_LOGI(TAG, "Alerts[%d] %s: %d active", i, s_cfg.locations[i].name,
                                 alert_scratch->count);
                    }
                } else {
                    ESP_LOGW(TAG, "Alerts[%d] %s failed; keeping previous",
                             i, s_cfg.locations[i].name);
                }
            }

            /* Fill the overview row-by-row (forecast + alert badge) as each
             * location lands, and keep the selected detail screen's banner
             * current the moment its own alert fetch lands. */
            if (s_view_index == active_view && esp_lv_adapter_lock(-1) == ESP_OK) {
                if (overview) {
                    update_overview();
                    lv_label_set_text(s_status_label, overview_loading() ? "Henter oversikt..." : "");
                } else if (i == sel) {
                    update_alert_banner(sel);
                }
                esp_lv_adapter_unlock();
            }
        }

        if (s_view_index != active_view) {
            continue;
        }

        if (overview) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                update_overview();
                lv_label_set_text(s_status_label, overview_loading() ? "Henter oversikt..." : "");
                esp_lv_adapter_unlock();
            }
        } else if (radar || ships) {
            /* Already handled above; nothing weather-related to draw here. */
        } else {
            /* Keeps the banner in sync with whatever the fetch loop below
             * last landed for this location, even on a pass that finds
             * nothing else to do (e.g. the forecast is still fresh). */
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                update_alert_banner(sel);
                esp_lv_adapter_unlock();
            }

            /* Nowcast refreshes every 5 min upstream - fetch it every cycle. */
            double lat = atof(s_cfg.locations[sel].lat);
            double lon = atof(s_cfg.locations[sel].lon);
            bool nc_ok = (yr_client_fetch_nowcast(lat, lon, nowcast) == ESP_OK &&
                          nowcast->valid && nowcast->radar_ok && nowcast->point_count > 0);

            if (s_view_index != active_view) {
                continue;
            }

            if (s_fc_valid[sel] && s_fc_cache[sel]->point_count > 0) {
                const yr_forecast_t *to_render = s_fc_cache[sel];
                if (nc_ok) {
                    merge_nowcast(merged, s_fc_cache[sel], nowcast);
                    to_render = merged;
                    ESP_LOGI(TAG, "Nowcast merged for %s: %d steps -> %d points",
                             s_cfg.locations[sel].name, nowcast->point_count,
                             merged->point_count);
                }
                if (to_render->point_count > 0) {
                    /* Every chart/label below positions by index, so put the
                     * points on a uniform time grid first (see
                     * resample_uniform_time) - otherwise the nowcast-merged
                     * series' densely-sampled first ~2h would visually eat
                     * as much of the x-axis as several hours further out. */
                    resample_uniform_time(resampled, to_render);
                    if (esp_lv_adapter_lock(-1) == ESP_OK) {
                        lv_label_set_text(s_status_label, "");
                        update_ui_with_forecast(resampled);
                        esp_lv_adapter_unlock();
                    }
                }
            } else if (esp_lv_adapter_lock(-1) == ESP_OK) {
                lv_label_set_text(s_status_label,
                                  "Kunne ikke hente v\xC3\xA6rvarsel. Pr\xC3\xB8ver igjen...");
                esp_lv_adapter_unlock();
            }
        }

        /* Poll on the nowcast cadence once something is on screen; retry fast
         * while still waiting for the first data. A tap notifies us, cutting
         * the wait short. */
        bool ready = overview ? !overview_loading() : s_fc_valid[sel];
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(radar ? ADSB_POLL_MS
                                               : ships ? SHIP_POLL_MS
                                               : (ready ? NOWCAST_REFRESH_INTERVAL_MS
                                                        : WEATHER_RETRY_INTERVAL_MS)));
    }
}

void app_main(void)
{
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
    app_config_load(&s_cfg);
    yr_client_set_contact_email(s_cfg.yr_email);

    /* Resume on whatever screen was showing before this boot (see
     * app_config_save_last_view) rather than always starting at the
     * overview. Clamped in case the location count shrank since. */
    build_stops();
    s_view_index = app_config_load_last_view();
    if (s_view_index >= s_stop_count) {
        s_view_index = 0;
    }

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;
    const uint8_t frame_buffer_count = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_touch_handle_t touch_handle = NULL;

    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init(
        frame_buffer_count,
        &panel_handle,
        &touch_handle));
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

    /* This task must keep its stack in internal RAM: it calls
     * nvs_flash_init()/esp_wifi via wifi_connect_sta(), and flash/NVS
     * access briefly freezes the cache, which asserts that the calling
     * task's own stack isn't in PSRAM (it would become unreadable). */
    xTaskCreate(yr_weather_task, "yr_weather", YR_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, &s_yr_task);
}
