#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmap_generate_fonts.h"
#include "nvs_flash.h"
#include "waveshare_rgb_lcd_port.h"
#include "app_config.h"
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
#define PRECIP_MARKER_POOL 6
#define WIND_MARKER_POOL 5

/* Overview screen: a table with one row per location and OV_COLS time columns
 * OV_STEP_H hours apart. Each cell shows the weather icon, the temperature at
 * that hour and the precipitation summed over the following OV_STEP_H hours.
 * It is the first stop when cycling with a left-half tap (only shown when two
 * or more locations are configured). */
#define OV_COLS     4
#define OV_STEP_H   6
#define OV_X        10
#define OV_NAME_W   118
#define OV_COL_W    165            /* (800 - OV_X*2 - OV_NAME_W) / OV_COLS   */
#define OV_TITLE_Y  6
#define OV_HDR_Y    42
#define OV_BODY_Y   64
/* 76, not 80: at the 5-location max that leaves a clear strip at the very
 * bottom of the screen for the IP-address label. */
#define OV_ROW_H    76
#define OV_ICON     34

static const lv_font_t *s_font_body;
static const lv_font_t *s_font_large;

/* Runtime settings (WiFi + forecast locations), from NVS via the setup portal
 * or the compiled-in defaults. Loaded once in app_main. */
static app_config_t s_cfg;

/* Which "stop" is on screen, advanced by a left-half tap. 0 is the overview
 * table (only a real stop when >= 2 locations); 1..location_count are the
 * per-location detail screens. The weather task watches this and re-renders. */
static volatile int s_view_index;
static TaskHandle_t s_yr_task;

static lv_obj_t *s_status_label;
static lv_obj_t *s_tap_layer;     /* full-screen tap catcher; also the night-dim overlay */
static lv_obj_t *s_detail_root;   /* holds every per-location detail widget  */
static lv_obj_t *s_overview_root; /* holds the all-locations overview table   */
static lv_obj_t *s_location_label;
static lv_obj_t *s_updated_label;

/* Overview table widgets (built only when >= 2 locations). */
static lv_obj_t *s_ov_title;
static lv_obj_t *s_ov_ip_label;   /* bottom-right: the address to browse to for setup */
static lv_obj_t *s_ov_heap_label; /* bottom-left: free internal-DRAM bytes */
static lv_obj_t *s_ov_hdr[OV_COLS];
static lv_obj_t *s_ov_name[APP_CONFIG_MAX_LOCATIONS];
static lv_obj_t *s_ov_icon[APP_CONFIG_MAX_LOCATIONS][OV_COLS];
static lv_obj_t *s_ov_cell[APP_CONFIG_MAX_LOCATIONS][OV_COLS];

/* Per-location hourly forecast cache (PSRAM), kept warm for every location so
 * the overview can show them all at once. s_fc_cache[i] is allocated in the
 * weather task; s_fc_valid[i] gates reads; s_fc_tk[i] is its last refresh. */
static yr_forecast_t *s_fc_cache[APP_CONFIG_MAX_LOCATIONS];
static bool s_fc_valid[APP_CONFIG_MAX_LOCATIONS];
static TickType_t s_fc_tk[APP_CONFIG_MAX_LOCATIONS];

static lv_obj_t *s_precip_chart;
static lv_chart_series_t *s_precip_series;
static lv_obj_t *s_temp_line;
static lv_obj_t *s_temp_markers[TEMP_MARKER_POOL];
static lv_obj_t *s_precip_markers[PRECIP_MARKER_POOL];

static lv_obj_t *s_wind_chart;
static lv_chart_series_t *s_wind_series;
static lv_obj_t *s_wind_markers[WIND_MARKER_POOL];
static lv_obj_t *s_wind_dir_arrows[NUM_HOUR_LABELS];

static lv_obj_t *s_hour_labels[NUM_HOUR_LABELS];
static lv_obj_t *s_icon_slots[NUM_HOUR_LABELS];

static int32_t s_precip_chart_data[YR_FORECAST_MAX_POINTS]; /* millimeters * 10 */
static int32_t s_wind_chart_data[YR_FORECAST_MAX_POINTS];   /* m/s * 10 */
static lv_point_precise_t s_temp_line_points[YR_FORECAST_MAX_POINTS];

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

/* Tap anywhere on the screen: advance to the next stop (overview -> location 1
 * -> location 2 -> ... -> overview) and wake the weather task so it
 * re-renders / refetches. Runs in the LVGL context (which already holds the
 * adapter lock), so it only pokes volatiles + a notify. */
static void screen_touch_cb(lv_event_t *e)
{
    (void)e;
    if (s_cfg.location_count <= 1) {
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

    int stops = s_cfg.location_count + 1; /* overview + one per location */
    int next = s_view_index + 1;
    if (next >= stops) {
        next = 0;
    }
    s_view_index = next;
    ESP_LOGI(TAG, "Tap: view %d (0=overview, 1..%d=locations)",
             next, s_cfg.location_count);
    if (s_yr_task != NULL) {
        xTaskNotifyGive(s_yr_task);
    }
}

/* Show either the overview table or the per-location detail screen. The
 * status label and tap layer sit above both and are left alone. */
static void show_overview(bool on)
{
    lv_obj_t *shown = on ? s_overview_root : s_detail_root;
    lv_obj_t *hidden = on ? s_detail_root : s_overview_root;
    if (hidden) {
        lv_obj_add_flag(hidden, LV_OBJ_FLAG_HIDDEN);
    }
    if (shown) {
        lv_obj_clear_flag(shown, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Build the overview table into `root`: a title, a header row of clock hours
 * (filled in each refresh), then one row per location with a name cell and
 * OV_COLS cells of {weather icon, temperature, precipitation}. Only called
 * when at least two locations are configured. */
static void build_overview(lv_obj_t *root)
{
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

    for (int i = 0; i < s_cfg.location_count; i++) {
        int row_y = OV_BODY_Y + i * OV_ROW_H;

        s_ov_name[i] = lv_label_create(root);
        lv_obj_set_pos(s_ov_name[i], OV_X, row_y + OV_ICON / 2 - 4);
        lv_obj_set_width(s_ov_name[i], OV_NAME_W - 4);
        lv_obj_set_style_text_align(s_ov_name[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(s_ov_name[i], LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(s_ov_name[i], s_cfg.locations[i].name);

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
    lv_obj_set_style_text_color(s_ov_ip_label, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_ov_ip_label, LV_ALIGN_BOTTOM_RIGHT, -OV_X, -4);
    lv_label_set_text(s_ov_ip_label, "");

    /* Free internal-DRAM bytes, the figure this project's memory work has
     * been tracking throughout - a running diagnostic, not user-facing data,
     * so it's a footnote like the IP label. */
    s_ov_heap_label = lv_label_create(root);
    lv_obj_set_style_text_color(s_ov_heap_label, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_ov_heap_label, LV_ALIGN_BOTTOM_LEFT, OV_X, -4);
    lv_label_set_text(s_ov_heap_label, "");
}

static void build_ui(lv_obj_t *screen)
{
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
     * specific location's detail screen, show its name from the first frame
     * rather than always location 0's - the weather task corrects this
     * itself moments later regardless, once WiFi is up. */
    int initial_loc = (s_cfg.location_count >= 2 && s_view_index > 0) ? (s_view_index - 1) : 0;

    s_location_label = lv_label_create(s_detail_root);
    lv_obj_set_style_text_font(s_location_label, s_font_large, 0);
    lv_obj_set_pos(s_location_label, 12, 4);
    lv_label_set_text(s_location_label, s_cfg.locations[initial_loc].name);

    s_updated_label = lv_label_create(s_detail_root);
    lv_obj_align(s_updated_label, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_label_set_text(s_updated_label, "");

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

    /* Precipitation bar chart acts as the single visual chart frame
     * (background, border, gridlines); the temperature line is overlaid
     * directly on top of it to make the two read as one merged chart. */
    s_precip_chart = lv_chart_create(s_detail_root);
    lv_obj_set_pos(s_precip_chart, CHART_X, CHART_Y);
    lv_obj_set_size(s_precip_chart, CHART_W, CHART_H);
    lv_obj_set_style_pad_left(s_precip_chart, 4, 0);
    lv_obj_set_style_pad_right(s_precip_chart, 4, 0);
    lv_chart_set_type(s_precip_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(s_precip_chart, 4, NUM_HOUR_LABELS - 1);
    lv_chart_set_point_count(s_precip_chart, YR_FORECAST_MAX_POINTS);
    lv_chart_set_axis_range(s_precip_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
    s_precip_series = lv_chart_add_series(s_precip_chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);

    s_temp_line = lv_line_create(s_detail_root);
    lv_obj_set_pos(s_temp_line, CHART_X, CHART_Y);
    lv_obj_set_size(s_temp_line, CHART_W, CHART_H);
    lv_obj_set_style_bg_opa(s_temp_line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_temp_line, 0, 0);
    lv_obj_set_style_line_width(s_temp_line, 3, 0);
    lv_obj_set_style_line_color(s_temp_line, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_line_rounded(s_temp_line, true, 0);
    lv_obj_clear_flag(s_temp_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_temp_line, LV_OBJ_FLAG_CLICKABLE);
    /* Actual point count is set each refresh in update_ui_with_forecast. */
    lv_line_set_points_mutable(s_temp_line, s_temp_line_points, 0);

    /* Value markers for temperature and precipitation extrema, positioned
     * directly on the chart each refresh. Both are pools: how many are used
     * depends on the forecast (see place_temp_markers / place_precip_markers).
     * Any left over are kept hidden. */
    for (int i = 0; i < TEMP_MARKER_POOL; i++) {
        s_temp_markers[i] = lv_label_create(s_detail_root);
        lv_obj_set_style_text_color(s_temp_markers[i], lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);
        lv_label_set_text(s_temp_markers[i], "");
        lv_obj_add_flag(s_temp_markers[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < PRECIP_MARKER_POOL; i++) {
        s_precip_markers[i] = lv_label_create(s_detail_root);
        lv_obj_set_style_text_color(s_precip_markers[i], lv_palette_darken(LV_PALETTE_BLUE, 2), 0);
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

    for (int i = 0; i < NUM_HOUR_LABELS; i++) {
        s_wind_dir_arrows[i] = lv_image_create(wind_dir_row);
        lv_obj_set_size(s_wind_dir_arrows[i], WIND_ARROW_SIZE, WIND_ARROW_SIZE);
        lv_image_set_src(s_wind_dir_arrows[i], "F:arrow.png");
        lv_image_set_inner_align(s_wind_dir_arrows[i], LV_IMAGE_ALIGN_CENTER);
        lv_image_set_pivot(s_wind_dir_arrows[i], WIND_ARROW_SIZE / 2, WIND_ARROW_SIZE / 2);
        lv_obj_add_flag(s_wind_dir_arrows[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Wind-speed bar chart (m/s), same x-scale as the main chart above. */
    s_wind_chart = lv_chart_create(s_detail_root);
    lv_obj_set_pos(s_wind_chart, CHART_X, WIND_CHART_Y);
    lv_obj_set_size(s_wind_chart, CHART_W, WIND_CHART_H);
    lv_obj_set_style_pad_left(s_wind_chart, 4, 0);
    lv_obj_set_style_pad_right(s_wind_chart, 4, 0);
    lv_chart_set_type(s_wind_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(s_wind_chart, 2, NUM_HOUR_LABELS - 1);
    lv_chart_set_point_count(s_wind_chart, YR_FORECAST_MAX_POINTS);
    lv_chart_set_axis_range(s_wind_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    s_wind_series = lv_chart_add_series(s_wind_chart, lv_palette_main(LV_PALETTE_TEAL),
                                       LV_CHART_AXIS_PRIMARY_Y);

    /* Wind-speed value markers - same pooled scheme as the precipitation
     * markers (see place_wind_markers / place_precip_markers). */
    for (int i = 0; i < WIND_MARKER_POOL; i++) {
        s_wind_markers[i] = lv_label_create(s_detail_root);
        lv_obj_set_style_text_color(s_wind_markers[i], lv_palette_darken(LV_PALETTE_TEAL, 2), 0);
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

    /* The overview table is only a cycling stop with two or more locations. */
    if (s_cfg.location_count >= 2) {
        build_overview(s_overview_root);
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

    /* Start on whichever screen s_view_index was restored to (the overview
     * unless a specific location was last shown before the previous reboot),
     * or the single location's detail screen if there's only one. */
    show_overview(s_cfg.location_count >= 2 && s_view_index == 0);
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
static float pt_precip(const yr_forecast_point_t *p) { return p->precipitation_mm; }
static float pt_wind(const yr_forecast_point_t *p) { return p->wind_speed_ms; }

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

/* Precipitation value markers. Labels the global wettest hour, then adds a
 * label at each further local precipitation peak that falls at least
 * MARKER_MIN_GAP_H hours after the previously shown precip label, so a long
 * on/off rain spell gets called out without crowding. Each label consolidates
 * to the wettest hour within the next window (snap_to_better_extremum), so it
 * lands on the tallest bar of its shower, not the first bar of it. Dry hours
 * are never marked; an all-dry forecast hides every pool label. */
static void place_precip_markers(const yr_forecast_t *fc, int precip_max_idx, int32_t precip_range_max)
{
    int used = 0;

    if (round_to_int(fc->points[precip_max_idx].precipitation_mm * 10.0f) > 0) {
        int32_t precip_axis_max = precip_range_max * PRECIP_AXIS_COMPRESSION;
        /* One window before "now" - see place_temp_markers. */
        int64_t last_shown_epoch = fc->points[0].epoch_utc - (int64_t)MARKER_MIN_GAP_H * 3600;

        for (int i = 0; i < fc->point_count && used < PRECIP_MARKER_POOL; i++) {
            float mm = fc->points[i].precipitation_mm;
            if (round_to_int(mm * 10.0f) <= 0) {
                continue;
            }

            float prev = (i > 0) ? fc->points[i - 1].precipitation_mm : -1.0f;
            float next = (i < fc->point_count - 1) ? fc->points[i + 1].precipitation_mm : -1.0f;
            bool local_peak = (mm >= prev && mm >= next && (mm > prev || mm > next));
            bool is_global = (i == precip_max_idx);
            if (!is_global && !local_peak) {
                continue;
            }

            int64_t epoch = fc->points[i].epoch_utc;
            if (!is_global && (epoch - last_shown_epoch) < (int64_t)MARKER_MIN_GAP_H * 3600) {
                continue;
            }

            int m = snap_to_better_extremum(fc, i, true, pt_precip);

            int32_t x = (fc->point_count > 1)
                            ? (int32_t)m * (CHART_W - 1) / (fc->point_count - 1)
                            : 0;
            int32_t y = CHART_H - (int32_t)(((float)s_precip_chart_data[m] / (float)precip_axis_max) * CHART_H);

            lv_obj_t *label = s_precip_markers[used++];
            lv_label_set_text_fmt(label, "%.1f mm", (double)fc->points[m].precipitation_mm);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
            place_marker_label(label, CHART_X + x, CHART_Y + y, true);

            last_shown_epoch = fc->points[m].epoch_utc;
            if (m > i) {
                i = m; /* don't re-label the span we snapped across */
            }
        }
    }

    for (int i = used; i < PRECIP_MARKER_POOL; i++) {
        lv_obj_add_flag(s_precip_markers[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Wind-speed value markers on the wind chart - the same scheme as
 * place_precip_markers: label the windiest hour, then each further local wind
 * peak that falls at least MARKER_MIN_GAP_H hours after the previously shown
 * wind label, each consolidated to the strongest gust in the next window. If
 * the whole forecast is calm every pool label stays hidden. */
static void place_wind_markers(const yr_forecast_t *fc, int wind_max_idx, int32_t wind_range_max)
{
    int used = 0;

    if (round_to_int(fc->points[wind_max_idx].wind_speed_ms * 10.0f) > 0) {
        /* One window before "now" - see place_temp_markers. */
        int64_t last_shown_epoch = fc->points[0].epoch_utc - (int64_t)MARKER_MIN_GAP_H * 3600;

        for (int i = 0; i < fc->point_count && used < WIND_MARKER_POOL; i++) {
            float ws = fc->points[i].wind_speed_ms;

            float prev = (i > 0) ? fc->points[i - 1].wind_speed_ms : -1.0f;
            float next = (i < fc->point_count - 1) ? fc->points[i + 1].wind_speed_ms : -1.0f;
            bool local_peak = (ws >= prev && ws >= next && (ws > prev || ws > next));
            bool is_global = (i == wind_max_idx);
            if (!is_global && !local_peak) {
                continue;
            }

            int64_t epoch = fc->points[i].epoch_utc;
            if (!is_global && (epoch - last_shown_epoch) < (int64_t)MARKER_MIN_GAP_H * 3600) {
                continue;
            }

            int m = snap_to_better_extremum(fc, i, true, pt_wind);

            int32_t x = (fc->point_count > 1)
                            ? (int32_t)m * (CHART_W - 1) / (fc->point_count - 1)
                            : 0;
            int32_t y = WIND_CHART_H - (int32_t)(((float)s_wind_chart_data[m] /
                                                  (float)wind_range_max) * WIND_CHART_H);

            lv_obj_t *label = s_wind_markers[used++];
            lv_label_set_text_fmt(label, "%.0f m/s", (double)fc->points[m].wind_speed_ms);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
            place_marker_label(label, CHART_X + x, WIND_CHART_Y + y, true);

            last_shown_epoch = fc->points[m].epoch_utc;
            if (m > i) {
                i = m; /* don't re-label the span we snapped across */
            }
        }
    }

    for (int i = used; i < WIND_MARKER_POOL; i++) {
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

    int temp_min_idx = 0, temp_max_idx = 0, precip_max_idx = 0, wind_max_idx = 0;
    float temp_min = now->air_temperature_c;
    float temp_max = now->air_temperature_c;
    float precip_max = 0.0f;
    float wind_max = 0.0f;

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
        if (p->precipitation_mm > precip_max) {
            precip_max = p->precipitation_mm;
            precip_max_idx = i;
        }
        if (p->wind_speed_ms > wind_max) {
            wind_max = p->wind_speed_ms;
            wind_max_idx = i;
        }

        /* A dry hour draws no bar at all (LV_CHART_POINT_NONE), rather than a
         * flat zero-height stub sitting on the axis. */
        int32_t precip_tenths = round_to_int(p->precipitation_mm * 10.0f);
        s_precip_chart_data[i] = (precip_tenths > 0) ? precip_tenths : LV_CHART_POINT_NONE;

        s_wind_chart_data[i] = round_to_int(p->wind_speed_ms * 10.0f);
    }

    int32_t temp_range_min = round_to_int(temp_min) - 1;
    int32_t temp_range_max = round_to_int(temp_max) + 1;
    if (temp_range_max <= temp_range_min) {
        temp_range_max = temp_range_min + 1;
    }

    int32_t precip_range_max = round_to_int(precip_max * 10.0f) + 2;
    if (precip_range_max < 10) {
        precip_range_max = 10;
    }

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
    }
    /* Re-point every refresh: the merged series length varies (nowcast steps
     * + hourly points), and drawing the full YR_FORECAST_MAX_POINTS array
     * would trail a line back through the stale/zero tail entries. */
    lv_line_set_points_mutable(s_temp_line, s_temp_line_points,
                               fc->point_count > 1 ? (uint32_t)fc->point_count : 0);
    lv_obj_invalidate(s_temp_line);

    place_temp_markers(fc, temp_min_idx, temp_max_idx);
    place_precip_markers(fc, precip_max_idx, precip_range_max);

    /* Wind-speed bar chart: full m/s per unit, +2 m/s headroom, min 6 m/s so a
     * calm forecast still has a sensible axis. */
    int32_t wind_range_max = round_to_int(wind_max * 10.0f) + 20;
    if (wind_range_max < 60) {
        wind_range_max = 60;
    }
    lv_chart_set_point_count(s_wind_chart, fc->point_count);
    lv_chart_set_axis_range(s_wind_chart, LV_CHART_AXIS_PRIMARY_Y, 0, wind_range_max);
    lv_chart_set_series_ext_y_array(s_wind_chart, s_wind_series, s_wind_chart_data);
    lv_chart_refresh(s_wind_chart); /* force redraw - see the precip chart above */

    place_wind_markers(fc, wind_max_idx, wind_range_max);

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

/* Repaint the overview table from the per-location caches. Uses the most
 * recent cache's first point as "now" (the device has no wall clock), aligns
 * it to the hour, and for each column samples the nearest hourly point for
 * temperature + weather symbol and sums precipitation over the next OV_STEP_H
 * hours. Missing caches show dashes. Must be called under the adapter lock. */
static void update_overview(void)
{
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

    for (int i = 0; i < s_cfg.location_count; i++) {
        lv_label_set_text(s_ov_name[i], s_cfg.locations[i].name);

        const bool ok = s_fc_valid[i] && s_fc_cache[i]->point_count > 0;
        const yr_forecast_t *fc = ok ? s_fc_cache[i] : NULL;

        for (int c = 0; c < OV_COLS; c++) {
            lv_obj_t *icon = s_ov_icon[i][c];
            lv_obj_t *cell = s_ov_cell[i][c];

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

        const yr_forecast_point_t *nb = nearest_base_point(base, s->epoch_utc);
        if (s->has_instant_details) {
            p.air_temperature_c = s->air_temperature_c;
            p.wind_speed_ms = s->wind_speed_ms;
            p.wind_from_deg = s->wind_from_deg;
        } else {
            p.air_temperature_c = interp_base(base, s->epoch_utc, pt_temp);
            p.wind_speed_ms = interp_base(base, s->epoch_utc, pt_wind);
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

static void yr_weather_task(void *arg)
{
    /* Connects in station mode, or blocks forever in the setup portal (and
     * reboots when the form is saved). */
    wifi_provision_connect(&s_cfg, provision_status_cb);

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
     * hourly forecast for the overview. */
    yr_forecast_t *scratch = heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_SPIRAM);
    yr_forecast_t *merged = heap_caps_malloc(sizeof(*merged), MALLOC_CAP_SPIRAM);
    yr_forecast_t *resampled = heap_caps_malloc(sizeof(*resampled), MALLOC_CAP_SPIRAM);
    yr_nowcast_t *nowcast = heap_caps_malloc(sizeof(*nowcast), MALLOC_CAP_SPIRAM);
    bool caches_ok = (scratch != NULL && merged != NULL && resampled != NULL && nowcast != NULL);
    for (int i = 0; i < s_cfg.location_count; i++) {
        s_fc_cache[i] = heap_caps_malloc(sizeof(yr_forecast_t), MALLOC_CAP_SPIRAM);
        if (s_fc_cache[i] == NULL) {
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
            overview = (s_cfg.location_count >= 2 && want_view == 0);
            sel = overview ? 0
                : (s_cfg.location_count >= 2 ? want_view - 1 : 0);
            force_sel_refetch = !overview;

            /* So a reboot of any kind - nightly, power cycle, crash - comes
             * back showing this same screen instead of the overview. */
            app_config_save_last_view((uint8_t)want_view);

            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                show_overview(overview);
                if (overview) {
                    if (any_cache_valid()) {
                        lv_label_set_text(s_status_label, "");
                        update_overview();
                    } else {
                        lv_label_set_text(s_status_label, "Henter oversikt...");
                    }
                } else {
                    const app_location_t *loc = &s_cfg.locations[sel];
                    if (s_cfg.location_count > 1) {
                        lv_label_set_text_fmt(s_location_label, "%s  %d/%d", loc->name,
                                              sel + 1, s_cfg.location_count);
                    } else {
                        lv_label_set_text(s_location_label, loc->name);
                    }
                    /* Never render from cache here, even if valid - wait for
                     * the fresh forecast+nowcast fetch below so the graph
                     * doesn't flash an old forecast before the nowcast lands. */
                    lv_label_set_text(s_status_label, "Henter v\xC3\xA6rvarsel...");
                }
                esp_lv_adapter_unlock();
            }
        }

        TickType_t now_tk = xTaskGetTickCount(); /* unsigned - wrap-safe deltas */

        /* Refresh any stale or missing location forecast. The selected one is
         * done first so the detail view updates promptly; the rest keep the
         * overview warm. N <= 5 and the cadence is 10 min, so this is a fetch
         * or two per wake at most. */
        for (int k = 0; k < s_cfg.location_count; k++) {
            if (s_view_index != active_view) {
                break; /* view changed mid-scan - restart the loop */
            }
            int i = (sel + k) % s_cfg.location_count;
            bool stale = !s_fc_valid[i] || (i == sel && force_sel_refetch) ||
                         (now_tk - s_fc_tk[i]) >= pdMS_TO_TICKS(WEATHER_REFRESH_INTERVAL_MS);
            if (!stale) {
                continue;
            }

            double lat = atof(s_cfg.locations[i].lat);
            double lon = atof(s_cfg.locations[i].lon);
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

            /* Fill the overview row-by-row as each location lands. */
            if (overview && s_view_index == active_view &&
                esp_lv_adapter_lock(-1) == ESP_OK) {
                if (any_cache_valid()) {
                    lv_label_set_text(s_status_label, "");
                    update_overview();
                }
                esp_lv_adapter_unlock();
            }
        }

        if (s_view_index != active_view) {
            continue;
        }

        if (overview) {
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                if (any_cache_valid()) {
                    lv_label_set_text(s_status_label, "");
                    update_overview();
                } else {
                    lv_label_set_text(s_status_label, "Henter oversikt...");
                }
                esp_lv_adapter_unlock();
            }
        } else {
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
        bool ready = overview ? any_cache_valid() : s_fc_valid[sel];
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ready ? NOWCAST_REFRESH_INTERVAL_MS
                                                     : WEATHER_RETRY_INTERVAL_MS));
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

    /* Resume on whatever screen was showing before this boot (see
     * app_config_save_last_view) rather than always starting at the
     * overview. Clamped in case the location count shrank since. */
    s_view_index = app_config_load_last_view();
    if (s_view_index > s_cfg.location_count) {
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
