#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

#define WEATHER_REFRESH_INTERVAL_MS (10 * 60 * 1000)
#define WEATHER_RETRY_INTERVAL_MS (20 * 1000)
#define NUM_HOUR_LABELS 8
#define YR_TASK_STACK_SIZE 8192

#define CHART_X 20
#define CHART_Y 150
#define CHART_W 760
#define CHART_H 250
/* Precipitation bars are drawn on a taller-than-needed axis so they only
 * occupy the bottom fraction of the shared chart, leaving the rest of the
 * height for the temperature line to read clearly. */
#define PRECIP_AXIS_COMPRESSION 3

static const lv_font_t *s_font_body;
static const lv_font_t *s_font_large;

static lv_obj_t *s_status_label;
static lv_obj_t *s_current_label;
static lv_obj_t *s_updated_label;

static lv_obj_t *s_precip_chart;
static lv_chart_series_t *s_precip_series;
static lv_obj_t *s_temp_line;
static lv_obj_t *s_temp_max_label;
static lv_obj_t *s_temp_min_label;
static lv_obj_t *s_precip_max_label;

static lv_obj_t *s_hour_labels[NUM_HOUR_LABELS];
static lv_obj_t *s_icon_slots[NUM_HOUR_LABELS];

static int32_t s_precip_chart_data[YR_FORECAST_MAX_POINTS]; /* millimeters * 10 */
static lv_point_precise_t s_temp_line_points[YR_FORECAST_MAX_POINTS];

typedef enum {
    WICON_CLEAR,
    WICON_FAIR,
    WICON_CLOUDY,
    WICON_FOG,
    WICON_RAIN,
    WICON_SLEET,
    WICON_SNOW,
    WICON_THUNDER,
} weather_icon_t;

typedef struct {
    weather_icon_t icon;
    char text_no[64];
} symbol_info_t;

static int32_t round_to_int(float v)
{
    return (int32_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
}

static bool strip_suffix(char *word, const char *suffix)
{
    size_t wlen = strlen(word);
    size_t slen = strlen(suffix);
    if (wlen > slen && strcmp(word + wlen - slen, suffix) == 0) {
        word[wlen - slen] = '\0';
        return true;
    }
    return false;
}

static bool strip_prefix(char *word, const char *prefix)
{
    size_t plen = strlen(prefix);
    size_t wlen = strlen(word);
    if (wlen > plen && strncmp(word, prefix, plen) == 0) {
        memmove(word, word + plen, wlen - plen + 1);
        return true;
    }
    return false;
}

/* MET Norway symbol codes are built from a small, fixed vocabulary:
 * [heavy|light]<phenomenon>[showers][andthunder][_day|_night|_polartwilight].
 * Decomposing them (rather than a lookup table per combination) covers the
 * full real vocabulary with a Norwegian description and an icon category. */
static void parse_symbol(const char *symbol_code, symbol_info_t *out)
{
    out->icon = WICON_CLEAR;
    out->text_no[0] = '\0';

    if (symbol_code[0] == '\0') {
        return;
    }

    char word[48];
    snprintf(word, sizeof(word), "%s", symbol_code);

    static const char *day_suffixes[] = { "_day", "_night", "_polartwilight" };
    for (size_t i = 0; i < sizeof(day_suffixes) / sizeof(day_suffixes[0]); i++) {
        if (strip_suffix(word, day_suffixes[i])) {
            break;
        }
    }

    if (strcmp(word, "clearsky") == 0) {
        out->icon = WICON_CLEAR;
        snprintf(out->text_no, sizeof(out->text_no), "Klarv\xC3\xA6r");
        return;
    }
    if (strcmp(word, "fair") == 0) {
        out->icon = WICON_FAIR;
        snprintf(out->text_no, sizeof(out->text_no), "Lettskyet");
        return;
    }
    if (strcmp(word, "partlycloudy") == 0) {
        out->icon = WICON_FAIR;
        snprintf(out->text_no, sizeof(out->text_no), "Delvis skyet");
        return;
    }
    if (strcmp(word, "cloudy") == 0) {
        out->icon = WICON_CLOUDY;
        snprintf(out->text_no, sizeof(out->text_no), "Skyet");
        return;
    }
    if (strcmp(word, "fog") == 0) {
        out->icon = WICON_FOG;
        snprintf(out->text_no, sizeof(out->text_no), "T\xC3\xA5ke");
        return;
    }

    bool thunder = strip_suffix(word, "andthunder");
    bool heavy = strip_prefix(word, "heavy");
    bool light = !heavy && strip_prefix(word, "light");
    bool showers = strip_suffix(word, "showers");

    const char *noun;
    weather_icon_t icon;
    if (strcmp(word, "rain") == 0) {
        noun = "regn";
        icon = WICON_RAIN;
    } else if (strcmp(word, "sleet") == 0) {
        noun = "sludd";
        icon = WICON_SLEET;
    } else if (strcmp(word, "snow") == 0) {
        noun = "sn\xC3\xB8";
        icon = WICON_SNOW;
    } else {
        /* Unrecognized code: fall back to the raw (suffix-stripped) name
         * rather than guessing further. */
        snprintf(out->text_no, sizeof(out->text_no), "%s", word);
        if (out->text_no[0]) {
            out->text_no[0] = (char)toupper((unsigned char)out->text_no[0]);
        }
        return;
    }
    if (thunder) {
        icon = WICON_THUNDER;
    }
    out->icon = icon;

    char body[40];
    snprintf(body, sizeof(body), "%s%s", noun, showers ? "byger" : "");

    if (heavy) {
        snprintf(out->text_no, sizeof(out->text_no), "Kraftig %s%s", body, thunder ? " med torden" : "");
    } else if (light) {
        snprintf(out->text_no, sizeof(out->text_no), "Lett %s%s", body, thunder ? " med torden" : "");
    } else {
        body[0] = (char)toupper((unsigned char)body[0]);
        snprintf(out->text_no, sizeof(out->text_no), "%s%s", body, thunder ? " med torden" : "");
    }
}

static lv_obj_t *add_shape_circle(lv_obj_t *parent, int32_t d, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_size(o, d, d);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *add_shape_rect(lv_obj_t *parent, int32_t w, int32_t h, int32_t radius, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void draw_weather_icon(lv_obj_t *slot, weather_icon_t icon)
{
    lv_obj_clean(slot);

    lv_color_t cloud_color = lv_palette_main(LV_PALETTE_GREY);
    lv_color_t sun_color = lv_palette_main(LV_PALETTE_ORANGE);
    lv_color_t rain_color = lv_palette_main(LV_PALETTE_BLUE);
    lv_color_t snow_color = lv_color_white();
    lv_color_t thunder_color = lv_palette_main(LV_PALETTE_YELLOW);

    switch (icon) {
    case WICON_CLEAR: {
        lv_obj_t *sun = add_shape_circle(slot, 20, sun_color);
        lv_obj_center(sun);
        break;
    }
    case WICON_FAIR: {
        lv_obj_t *sun = add_shape_circle(slot, 14, sun_color);
        lv_obj_align(sun, LV_ALIGN_TOP_LEFT, 1, 1);
        lv_obj_t *cloud = add_shape_rect(slot, 22, 12, 6, cloud_color);
        lv_obj_align(cloud, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
        break;
    }
    case WICON_CLOUDY: {
        lv_obj_t *cloud = add_shape_rect(slot, 24, 14, 7, cloud_color);
        lv_obj_center(cloud);
        break;
    }
    case WICON_FOG: {
        for (int i = 0; i < 3; i++) {
            lv_obj_t *bar = add_shape_rect(slot, 20, 2, 1, cloud_color);
            lv_obj_align(bar, LV_ALIGN_CENTER, 0, (i - 1) * 6);
        }
        break;
    }
    case WICON_RAIN:
    case WICON_SLEET: {
        lv_obj_t *cloud = add_shape_rect(slot, 22, 12, 6, cloud_color);
        lv_obj_align(cloud, LV_ALIGN_TOP_MID, 0, 1);
        for (int i = 0; i < 3; i++) {
            lv_color_t c = (icon == WICON_SLEET && (i % 2 == 0)) ? snow_color : rain_color;
            lv_obj_t *drop = add_shape_rect(slot, 3, 7, 1, c);
            lv_obj_align(drop, LV_ALIGN_BOTTOM_MID, (i - 1) * 7, 0);
        }
        break;
    }
    case WICON_SNOW: {
        lv_obj_t *cloud = add_shape_rect(slot, 22, 12, 6, cloud_color);
        lv_obj_align(cloud, LV_ALIGN_TOP_MID, 0, 1);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *flake = add_shape_circle(slot, 4, snow_color);
            lv_obj_align(flake, LV_ALIGN_BOTTOM_MID, (i - 1) * 7, 0);
        }
        break;
    }
    case WICON_THUNDER: {
        lv_obj_t *cloud = add_shape_rect(slot, 22, 12, 6, lv_palette_darken(LV_PALETTE_GREY, 2));
        lv_obj_align(cloud, LV_ALIGN_TOP_MID, 0, 1);
        lv_obj_t *bolt = add_shape_rect(slot, 5, 10, 1, thunder_color);
        lv_obj_align(bolt, LV_ALIGN_BOTTOM_MID, 0, 0);
        break;
    }
    }
}

static void init_fonts(void)
{
    /* Mount the "fonts" SPIFFS partition (built by spiffs_create_partition_assets
     * in main/CMakeLists.txt) as the "F:" drive so LVGL's FreeType binding can
     * open the font by path. */
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

    s_status_label = lv_label_create(screen);
    lv_obj_set_pos(s_status_label, 12, 4);
    lv_label_set_text(s_status_label, "Kobler til WiFi...");

    s_current_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_current_label, s_font_large, 0);
    lv_obj_set_pos(s_current_label, 12, 24);
    lv_label_set_text(s_current_label, "");

    s_updated_label = lv_label_create(screen);
    lv_obj_align(s_updated_label, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_label_set_text(s_updated_label, "");

    lv_obj_t *icon_row = lv_obj_create(screen);
    lv_obj_set_pos(icon_row, CHART_X, 80);
    lv_obj_set_size(icon_row, CHART_W, 32);
    lv_obj_set_style_border_width(icon_row, 0, 0);
    lv_obj_set_style_pad_all(icon_row, 0, 0);
    lv_obj_set_style_bg_opa(icon_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(icon_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(icon_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < NUM_HOUR_LABELS; i++) {
        s_icon_slots[i] = lv_obj_create(icon_row);
        lv_obj_remove_style_all(s_icon_slots[i]);
        lv_obj_set_size(s_icon_slots[i], 28, 28);
        lv_obj_clear_flag(s_icon_slots[i], LV_OBJ_FLAG_SCROLLABLE);
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
    lv_line_set_points_mutable(s_temp_line, s_temp_line_points, YR_FORECAST_MAX_POINTS);

    /* Value markers for the highest/lowest temperature and highest
     * precipitation point, positioned directly on the chart each refresh. */
    s_temp_max_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_temp_max_label, lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);
    lv_label_set_text(s_temp_max_label, "");

    s_temp_min_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_temp_min_label, lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);
    lv_label_set_text(s_temp_min_label, "");

    s_precip_max_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_precip_max_label, lv_palette_darken(LV_PALETTE_BLUE, 2), 0);
    lv_label_set_text(s_precip_max_label, "");

    lv_obj_t *hour_row = lv_obj_create(screen);
    lv_obj_set_pos(hour_row, CHART_X, CHART_Y + CHART_H + 8);
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

static void update_ui_with_forecast(const yr_forecast_t *fc)
{
    const yr_forecast_point_t *now = &fc->points[0];

    symbol_info_t now_symbol;
    parse_symbol(now->symbol_code, &now_symbol);
    lv_label_set_text_fmt(s_current_label, "%.1f%sC   %s   Vind %.1f m/s",
                           (double)now->air_temperature_c, "\xC2\xB0", now_symbol.text_no,
                           (double)now->wind_speed_ms);
    lv_label_set_text_fmt(s_updated_label, "V\xC3\xA6rvarsel oppdatert: %s", fc->updated_time);

    int temp_min_idx = 0, temp_max_idx = 0, precip_max_idx = 0;
    float temp_min = now->air_temperature_c;
    float temp_max = now->air_temperature_c;
    float precip_max = 0.0f;

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

        s_precip_chart_data[i] = round_to_int(p->precipitation_mm * 10.0f);
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
    lv_obj_invalidate(s_temp_line);

    lv_label_set_text_fmt(s_temp_max_label, "%.1f%s", (double)temp_max, "\xC2\xB0");
    place_marker_label(s_temp_max_label, CHART_X + s_temp_line_points[temp_max_idx].x,
                        CHART_Y + s_temp_line_points[temp_max_idx].y, true);

    lv_label_set_text_fmt(s_temp_min_label, "%.1f%s", (double)temp_min, "\xC2\xB0");
    place_marker_label(s_temp_min_label, CHART_X + s_temp_line_points[temp_min_idx].x,
                        CHART_Y + s_temp_line_points[temp_min_idx].y, false);

    int32_t precip_axis_max = precip_range_max * PRECIP_AXIS_COMPRESSION;
    int32_t precip_max_x = (fc->point_count > 1)
                                ? (int32_t)precip_max_idx * (CHART_W - 1) / (fc->point_count - 1)
                                : 0;
    int32_t precip_max_y = CHART_H - (int32_t)(((float)s_precip_chart_data[precip_max_idx] / (float)precip_axis_max) * CHART_H);
    lv_label_set_text_fmt(s_precip_max_label, "%.1f mm", (double)precip_max);
    place_marker_label(s_precip_max_label, CHART_X + precip_max_x, CHART_Y + precip_max_y, true);

    for (int i = 0; i < NUM_HOUR_LABELS; i++) {
        int idx = (fc->point_count - 1) * i / (NUM_HOUR_LABELS - 1);
        lv_label_set_text(s_hour_labels[i], fc->points[idx].hour_minute);

        symbol_info_t sym;
        parse_symbol(fc->points[idx].symbol_code, &sym);
        draw_weather_icon(s_icon_slots[i], sym.icon);
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

    while (1) {
        /* Forecast buffer is small but there's no need for it to compete
         * with mbedtls/TLS for scarce internal DRAM - keep it in PSRAM. */
        yr_forecast_t *forecast = heap_caps_malloc(sizeof(yr_forecast_t), MALLOC_CAP_SPIRAM);
        if (forecast == NULL) {
            ESP_LOGE(TAG, "Out of memory allocating forecast buffer");
            vTaskDelay(pdMS_TO_TICKS(WEATHER_RETRY_INTERVAL_MS));
            continue;
        }

        esp_err_t err = yr_client_fetch_forecast(lat, lon, forecast);
        bool ok = (err == ESP_OK && forecast->valid && forecast->point_count > 0);

        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (ok) {
                lv_label_set_text(s_status_label, "");
                update_ui_with_forecast(forecast);
            } else {
                lv_label_set_text(s_status_label, "Kunne ikke hente v\xC3\xA6rvarsel. Pr\xC3\xB8ver igjen...");
            }
            esp_lv_adapter_unlock();
        }

        free(forecast);

        /* A failure right after boot is often transient (WiFi connection
         * setup and TLS both compete hard for scarce internal DRAM at that
         * moment) - retry soon rather than waiting a full refresh cycle. */
        vTaskDelay(pdMS_TO_TICKS(ok ? WEATHER_REFRESH_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS));
    }
}

void app_main(void)
{
    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_180;
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
