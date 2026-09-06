#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_mmap_assets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmap_generate_fonts.h"
#include "waveshare_rgb_lcd_port.h"
#include "wifi_connect.h"
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
 * labelled; precipitation: the global wettest hour is always labelled.
 * Further local extrema get a label only once at least MARKER_MIN_GAP_H
 * hours have passed since the previously shown marker of the same kind, so a
 * long forecast gets intermediate labels without them crowding. Pools are
 * sized for the worst case (48 h / 6 h, plus the globals). */
#define MARKER_MIN_GAP_H 6
/* When a marker is about to be placed, if a more extreme local extremum of
 * the same kind sits within this many hours, the label moves there instead -
 * even if that breaks the MARKER_MIN_GAP_H spacing. */
#define MARKER_SNAP_H 3
#define TEMP_MARKER_POOL 10
#define PRECIP_MARKER_POOL 9

static const lv_font_t *s_font_body;
static const lv_font_t *s_font_large;

static lv_obj_t *s_status_label;
static lv_obj_t *s_location_label;
static lv_obj_t *s_updated_label;

static lv_obj_t *s_precip_chart;
static lv_chart_series_t *s_precip_series;
static lv_obj_t *s_temp_line;
static lv_obj_t *s_temp_markers[TEMP_MARKER_POOL];
static lv_obj_t *s_precip_markers[PRECIP_MARKER_POOL];

static lv_obj_t *s_wind_chart;
static lv_chart_series_t *s_wind_series;
static lv_obj_t *s_wind_max_label;
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
        font_path, 14, ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL);
    ESP_ERROR_CHECK(esp_lv_adapter_ft_font_init(&body_cfg, &body_handle));
    s_font_body = esp_lv_adapter_ft_font_get(body_handle);
    assert(s_font_body != NULL);

    esp_lv_adapter_ft_font_handle_t large_handle = NULL;
    const esp_lv_adapter_ft_font_config_t large_cfg = ESP_LV_ADAPTER_FT_FONT_FILE_CONFIG(
        font_path, 24, ESP_LV_ADAPTER_FT_FONT_STYLE_NORMAL);
    ESP_ERROR_CHECK(esp_lv_adapter_ft_font_init(&large_cfg, &large_handle));
    s_font_large = esp_lv_adapter_ft_font_get(large_handle);
    assert(s_font_large != NULL);
}

static void build_ui(lv_obj_t *screen)
{
    lv_obj_set_style_text_font(screen, s_font_body, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    s_location_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_location_label, s_font_large, 0);
    lv_obj_set_pos(s_location_label, 12, 4);
    lv_label_set_text(s_location_label, CONFIG_EXAMPLE_YR_LOCATION_NAME);

    s_updated_label = lv_label_create(screen);
    lv_obj_align(s_updated_label, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_label_set_text(s_updated_label, "");

    lv_obj_t *icon_row = lv_obj_create(screen);
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
    s_precip_chart = lv_chart_create(screen);
    lv_obj_set_pos(s_precip_chart, CHART_X, CHART_Y);
    lv_obj_set_size(s_precip_chart, CHART_W, CHART_H);
    lv_obj_set_style_pad_left(s_precip_chart, 4, 0);
    lv_obj_set_style_pad_right(s_precip_chart, 4, 0);
    lv_chart_set_type(s_precip_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(s_precip_chart, 4, NUM_HOUR_LABELS - 1);
    lv_chart_set_point_count(s_precip_chart, YR_FORECAST_MAX_POINTS);
    lv_chart_set_axis_range(s_precip_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
    s_precip_series = lv_chart_add_series(s_precip_chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);

    s_temp_line = lv_line_create(screen);
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
        s_temp_markers[i] = lv_label_create(screen);
        lv_obj_set_style_text_color(s_temp_markers[i], lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);
        lv_label_set_text(s_temp_markers[i], "");
        lv_obj_add_flag(s_temp_markers[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < PRECIP_MARKER_POOL; i++) {
        s_precip_markers[i] = lv_label_create(screen);
        lv_obj_set_style_text_color(s_precip_markers[i], lv_palette_darken(LV_PALETTE_BLUE, 2), 0);
        lv_label_set_text(s_precip_markers[i], "");
        lv_obj_add_flag(s_precip_markers[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Wind direction: a row of arrows (one per sampled column) rotated to
     * point the way the wind blows, sitting just above the wind-speed chart. */
    lv_obj_t *wind_dir_row = lv_obj_create(screen);
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
    s_wind_chart = lv_chart_create(screen);
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

    s_wind_max_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_wind_max_label, lv_palette_darken(LV_PALETTE_TEAL, 2), 0);
    lv_label_set_text(s_wind_max_label, "");
    lv_obj_add_flag(s_wind_max_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hour_row = lv_obj_create(screen);
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

    /* Created last so it sits on top of the chart: loading/error text, shown
     * centered over the (empty) chart area until a forecast lands. */
    s_status_label = lv_label_create(screen);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_status_label, "Kobler til WiFi...");
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

/* A label was about to be placed at points[idx]. If a strictly more extreme
 * local extremum of the same kind (higher for a max, lower for a min) sits
 * within MARKER_SNAP_H hours after it, return that index instead so the
 * label lands on the real peak/trough. This deliberately overrides the
 * MARKER_MIN_GAP_H spacing rule. */
static int snap_to_better_extremum(const yr_forecast_t *fc, int idx, bool want_max,
                                   float (*get)(const yr_forecast_point_t *))
{
    int best = idx;
    float best_val = get(&fc->points[idx]);
    int64_t limit = fc->points[idx].epoch_utc + (int64_t)MARKER_SNAP_H * 3600;

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

/* Place the temperature value markers. Always labels the global high
 * (temp_max_idx, above the line) and global low (temp_min_idx, below); then
 * walks the series and adds a marker at each further local extremum whose
 * time is >= MARKER_MIN_GAP_H hours after the last marker already placed, so
 * intermediate peaks/dips get a value without crowding. Each marker snaps to
 * a better nearby extremum (see snap_to_better_extremum). Unused pool labels
 * are hidden. */
static void place_temp_markers(const yr_forecast_t *fc, int temp_min_idx, int temp_max_idx)
{
    int used = 0;
    int64_t last_shown_epoch = fc->points[0].epoch_utc;

    for (int i = 0; i < fc->point_count && used < TEMP_MARKER_POOL; i++) {
        const float t = fc->points[i].air_temperature_c;

        bool local_max = false;
        bool local_min = false;
        if (i > 0 && i < fc->point_count - 1) {
            float prev = fc->points[i - 1].air_temperature_c;
            float next = fc->points[i + 1].air_temperature_c;
            local_max = (t > prev && t >= next);
            local_min = (t < prev && t <= next);
        } else if (i == fc->point_count - 1 && i > 0) {
            float prev = fc->points[i - 1].air_temperature_c;
            local_max = (t > prev);
            local_min = (t < prev);
        } else if (i == 0 && fc->point_count > 1) {
            /* The leftmost point ("now") is a turning point whenever the
             * trend starts up or down from it - label it like an extremum. */
            float next = fc->points[1].air_temperature_c;
            local_max = (t > next);
            local_min = (t < next);
        }

        /* Globals and the leftmost point are shown regardless of spacing. */
        bool forced = (i == temp_min_idx || i == temp_max_idx || i == 0);
        if (!forced && !local_max && !local_min) {
            continue;
        }

        int64_t epoch = fc->points[i].epoch_utc;
        bool far_enough = (epoch - last_shown_epoch) >= (int64_t)MARKER_MIN_GAP_H * 3600;
        if (!forced && !far_enough) {
            continue;
        }

        bool above = (i == temp_max_idx) ? true
                   : (i == temp_min_idx) ? false
                   : local_max;

        int m = snap_to_better_extremum(fc, i, above, pt_temp);

        lv_obj_t *label = s_temp_markers[used++];
        lv_label_set_text_fmt(label, "%.1f%s", (double)fc->points[m].air_temperature_c, "\xC2\xB0");
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        place_marker_label(label, CHART_X + s_temp_line_points[m].x,
                            CHART_Y + s_temp_line_points[m].y, above);

        last_shown_epoch = fc->points[m].epoch_utc;
        if (m > i) {
            i = m; /* don't re-label the span we snapped across */
        }
    }

    for (int i = used; i < TEMP_MARKER_POOL; i++) {
        lv_obj_add_flag(s_temp_markers[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Precipitation value markers. Labels the global wettest hour, then adds a
 * label at each further local precipitation peak that falls at least
 * MARKER_MIN_GAP_H hours after the previously shown precip label, so a long
 * on/off rain spell gets called out without crowding. Dry hours are never
 * marked, and if the whole forecast is dry every pool label stays hidden. */
static void place_precip_markers(const yr_forecast_t *fc, int precip_max_idx, int32_t precip_range_max)
{
    int used = 0;

    if (round_to_int(fc->points[precip_max_idx].precipitation_mm * 10.0f) > 0) {
        int32_t precip_axis_max = precip_range_max * PRECIP_AXIS_COMPRESSION;
        int64_t last_shown_epoch = fc->points[0].epoch_utc;

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

    if (wind_max > 0.0f) {
        int32_t wx = (fc->point_count > 1)
                         ? (int32_t)wind_max_idx * (CHART_W - 1) / (fc->point_count - 1)
                         : 0;
        int32_t wy = WIND_CHART_H - (int32_t)(((float)s_wind_chart_data[wind_max_idx] /
                                               (float)wind_range_max) * WIND_CHART_H);
        lv_label_set_text_fmt(s_wind_max_label, "%.0f m/s", (double)wind_max);
        lv_obj_clear_flag(s_wind_max_label, LV_OBJ_FLAG_HIDDEN);
        place_marker_label(s_wind_max_label, CHART_X + wx, WIND_CHART_Y + wy, true);
    } else {
        lv_obj_add_flag(s_wind_max_label, LV_OBJ_FLAG_HIDDEN);
    }

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

static float pt_wind(const yr_forecast_point_t *p) { return p->wind_speed_ms; }

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

static void yr_weather_task(void *arg)
{
    wifi_connect_sta();

    /* Let WiFi's own connection-setup buffers settle before hitting it with
     * a large TLS handshake - the two compete hard for the same scarce
     * internal DRAM in the first moment after association. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_label_set_text(s_status_label, "Henter v\xC3\xA6rvarsel...");
        esp_lv_adapter_unlock();
    }

    const double lat = atof(CONFIG_EXAMPLE_YR_LATITUDE);
    const double lon = atof(CONFIG_EXAMPLE_YR_LONGITUDE);

    /* All in PSRAM - large, and no reason to compete with mbedtls/TLS for
     * scarce internal DRAM. `base` holds the last good hourly forecast
     * across iterations; `merged` is what actually gets rendered. */
    yr_forecast_t *base = heap_caps_malloc(sizeof(*base), MALLOC_CAP_SPIRAM);
    yr_forecast_t *merged = heap_caps_malloc(sizeof(*merged), MALLOC_CAP_SPIRAM);
    yr_nowcast_t *nowcast = heap_caps_malloc(sizeof(*nowcast), MALLOC_CAP_SPIRAM);
    if (base == NULL || merged == NULL || nowcast == NULL) {
        ESP_LOGE(TAG, "Out of memory allocating forecast buffers");
        free(base);
        free(merged);
        free(nowcast);
        vTaskDelete(NULL);
        return;
    }
    base->point_count = 0;

    bool have_forecast = false;
    TickType_t last_forecast_tk = 0;

    while (1) {
        TickType_t now_tk = xTaskGetTickCount(); /* unsigned - wrap-safe deltas */

        /* Refetch the slow-moving hourly forecast on its own long cadence
         * (or if we've never managed to get one). A failure keeps the last
         * good copy. */
        if (!have_forecast ||
            (now_tk - last_forecast_tk) >= pdMS_TO_TICKS(WEATHER_REFRESH_INTERVAL_MS)) {
            if (yr_client_fetch_forecast(lat, lon, base) == ESP_OK &&
                base->valid && base->point_count > 0) {
                have_forecast = true;
                last_forecast_tk = now_tk;
                ESP_LOGI(TAG, "Forecast updated: %d points, kl. %s (free heap: %u int / %u total)",
                         base->point_count, base->updated_hour_minute,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)esp_get_free_heap_size());
            }
        }

        /* The nowcast refreshes every 5 min upstream - fetch it every cycle. */
        bool nc_ok = (yr_client_fetch_nowcast(lat, lon, nowcast) == ESP_OK &&
                      nowcast->valid && nowcast->radar_ok && nowcast->point_count > 0);

        if (have_forecast) {
            const yr_forecast_t *to_render = base;
            if (nc_ok) {
                merge_nowcast(merged, base, nowcast);
                to_render = merged;
                ESP_LOGI(TAG, "Nowcast merged: %d steps @ kl. %s -> %d points",
                         nowcast->point_count, nowcast->updated_hour_minute, merged->point_count);
            }
            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                lv_label_set_text(s_status_label, "");
                update_ui_with_forecast(to_render);
                esp_lv_adapter_unlock();
            }
        } else if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_label_set_text(s_status_label, "Kunne ikke hente v\xC3\xA6rvarsel. Pr\xC3\xB8ver igjen...");
            esp_lv_adapter_unlock();
        }

        /* Poll on the nowcast cadence once we have something to show;
         * retry fast while still waiting for the first forecast. */
        vTaskDelay(pdMS_TO_TICKS(have_forecast ? NOWCAST_REFRESH_INTERVAL_MS
                                               : WEATHER_RETRY_INTERVAL_MS));
    }
}

void app_main(void)
{
    /* Europe/Oslo: CET (UTC+1), CEST (UTC+2) from the last Sunday of March
     * 02:00 to the last Sunday of October 03:00. Process-wide, so yr_client's
     * localtime_r() calls render the forecast's UTC timestamps in Norwegian
     * wall-clock time. Set before the weather task starts. */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

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
    xTaskCreate(yr_weather_task, "yr_weather", YR_TASK_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, NULL);
}
