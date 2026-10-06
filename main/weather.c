/* The weather screens: one per location with MET's forecast (yr_client),
 * nowcast and severe weather alerts (met_alerts_client), and the overview
 * table of all of them. The forecasts and alerts are cached per location
 * and refreshed on their own cadences by weather_poll. */

#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app.h"
#include "diag.h"
#include "sun.h"
#include "draw.h"
#include "aurora_client.h"
#include "met_alerts_client.h"
#include "watchdog.h"
#include "weather.h"
#include "yr_client.h"

static const char *TAG = "weather";

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
/* Yr's aurora forecast follows NOAA's Kp forecast, a few times a day. */
#define AURORA_REFRESH_INTERVAL_MS (60 * 60 * 1000)
/* Take every Nth nowcast step (5 min apart) into the merged series: every
 * 2nd = 10-minute resolution for the near term, still 6x finer than hourly
 * without over-compressing the rest of the chart. */
#define NOWCAST_MERGE_STRIDE 2
#define NUM_HOUR_LABELS 8

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

/* The bar images (see bars_t): inside the frames' borders, the precipitation
 * one only as tall as its bars can get. */
#define BARS_INSET      2
#define PRECIP_BARS_H   (CHART_H / PRECIP_AXIS_COMPRESSION + 1)
#define WIND_BARS_H     (WIND_CHART_H - 2 * BARS_INSET)
/* The bar behind (the precipitation max, the gust) at half the opacity of
 * the one in front: on white, exactly the palette's "lighten 3" shade. */
#define BARS_BACK_OPA   128

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
#define WIND_MARKER_GAP  12  /* px kept between wind markers, or the weaker is left out */
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
/* Locations that show weather, in order: the rows of the overview. */
static int s_weather_count;
static uint8_t s_wx_loc[APP_CONFIG_MAX_LOCATIONS]; /* weather index -> location */

/* PSRAM, all large. `s_scratch` receives each forecast fetch (yr_client
 * zeroes its output, so fetching straight into a cache would wipe the last
 * good copy on a network hiccup); `s_merged` holds the nowcast-spliced
 * series; `s_resampled` is what the detail view actually renders (see
 * resample_uniform_time). `s_alert_scratch` is the same for the alerts. */
static yr_forecast_t *s_scratch, *s_merged, *s_resampled;
static yr_nowcast_t *s_nowcast;
static met_alerts_t *s_alert_scratch;
static lv_obj_t *s_detail_root;   /* holds every per-location detail widget  */
static lv_obj_t *s_overview_root; /* holds the all-locations overview table   */
static lv_obj_t *s_location_label;
static lv_obj_t *s_updated_label;
/* Today's sunrise and sunset, left of s_updated_label (see sun_update), and
 * the night hours shaded in the two charts. */
#define NIGHT_BANDS 3
static lv_obj_t *s_sun_label;
static lv_color_t s_sun_colour;
/* What each location's last nowcast says about precipitation starting or
 * stopping (see rain_text), and when that nowcast came in. */
static yr_rain_change_t s_rain[APP_CONFIG_MAX_LOCATIONS];
static int64_t s_rain_at[APP_CONFIG_MAX_LOCATIONS];
static fetch_stamp_t s_rain_stamp[APP_CONFIG_MAX_LOCATIONS];
#define RAIN_NOTE_MAX_MS (15 * 60 * 1000) /* an older nowcast says nothing */
static lv_obj_t *s_night_main[NIGHT_BANDS], *s_night_wind[NIGHT_BANDS];
static lv_obj_t *s_alert_label; /* top-centre: the selected location's worst active alert, if any */
#define WX_CACHE_MAX_MS     (30 * 60 * 1000)
/* A forecast fetched this long ago is flagged as old on screen. */
#define WX_STALE_S          (30 * 60)
static yr_forecast_t *s_wx_shown[APP_CONFIG_MAX_LOCATIONS];
static fetch_stamp_t s_wx_shown_at[APP_CONFIG_MAX_LOCATIONS];

/* Overview table widgets (built only when >= 2 locations). */
static lv_obj_t *s_ov_title;
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
/* MET's Expires/Last-Modified for each cached forecast and alert list, and
 * for the one nowcast held (s_nc_loc's): see http_util.h. */
static EXT_RAM_BSS_ATTR http_cache_t s_fc_http[APP_CONFIG_MAX_LOCATIONS];
static EXT_RAM_BSS_ATTR http_cache_t s_alert_http[APP_CONFIG_MAX_LOCATIONS];
static http_cache_t s_nc_http;
static int s_nc_loc = -1;
static bool s_nc_held; /* s_nowcast holds a parsed nowcast for s_nc_loc */
static time_t s_fc_when[APP_CONFIG_MAX_LOCATIONS]; /* wall clock of that refresh, 0 if not synced */

/* Per-location severe weather alerts (PSRAM), same shape as the forecast
 * cache above and refreshed on its own cadence (see ALERT_REFRESH_INTERVAL_MS). */
static met_alerts_t *s_alert_cache[APP_CONFIG_MAX_LOCATIONS];
static bool s_alert_valid[APP_CONFIG_MAX_LOCATIONS];
static TickType_t s_alert_tk[APP_CONFIG_MAX_LOCATIONS];

/* Per-location aurora forecasts (PSRAM), for the weather screen only: the
 * hours with a good chance of seeing it are marked on the chart (see
 * aurora_update). */
static aurora_t *s_aurora_cache[APP_CONFIG_MAX_LOCATIONS];
static aurora_t *s_aurora_scratch;
static EXT_RAM_BSS_ATTR http_cache_t s_aurora_http[APP_CONFIG_MAX_LOCATIONS];
static bool s_aurora_valid[APP_CONFIG_MAX_LOCATIONS];
static bool s_aurora_tried[APP_CONFIG_MAX_LOCATIONS]; /* s_aurora_tk is the last try, even a failed one */
static TickType_t s_aurora_tk[APP_CONFIG_MAX_LOCATIONS];
#define AURORA_BANDS 3
static lv_obj_t *s_aurora_band[AURORA_BANDS], *s_aurora_label[AURORA_BANDS];

/* A pair of bar series - one bar in front of a taller, paler one behind it
 * (the precipitation min and max, the wind and its gust) - drawn as one A8
 * image in the one colour: the front bar opaque, the part of the back bar
 * above it at BARS_BACK_OPA. An lv_chart bar series would do, but it adds a
 * draw task per bar for every strip of the screen LVGL draws, whether the
 * bar reaches into the strip or not: some 150 tasks at once for these two
 * pairs, ~30 KB of internal DRAM, while an image is one task per strip. */
typedef struct {
    lv_obj_t *obj;
    uint8_t *px;       /* PSRAM, CHART_W x the object's height */
    lv_image_dsc_t img;
    lv_color_t color;
} bars_t;

/* The frames (background/border/gridlines) under the bars: charts with no
 * series of their own. The temperature line and the markers are on top. */
static lv_obj_t *s_precip_frame;
static bars_t s_precip_bars;
static lv_obj_t *s_temp_line;
static lv_obj_t *s_temp_markers[TEMP_MARKER_POOL];
static lv_obj_t *s_precip_markers[PRECIP_MARKER_POOL];

static lv_obj_t *s_wind_frame;
static bars_t s_wind_bars;
static lv_obj_t *s_wind_markers[WIND_MARKER_POOL];
static lv_obj_t *s_wind_dir_arrows[NUM_HOUR_LABELS];

static lv_obj_t *s_hour_labels[NUM_HOUR_LABELS];
static lv_obj_t *s_icon_slots[NUM_HOUR_LABELS];

/* The bars' values; LV_CHART_POINT_NONE for no bar. */
static EXT_RAM_BSS_ATTR int32_t s_precip_chart_data[YR_FORECAST_MAX_POINTS];     /* millimeters * 10 */
static EXT_RAM_BSS_ATTR int32_t s_precip_max_chart_data[YR_FORECAST_MAX_POINTS]; /* millimeters * 10 */
static EXT_RAM_BSS_ATTR int32_t s_wind_chart_data[YR_FORECAST_MAX_POINTS];   /* m/s * 10 */
static EXT_RAM_BSS_ATTR int32_t s_gust_chart_data[YR_FORECAST_MAX_POINTS];   /* m/s * 10 */

static void bars_draw_cb(lv_event_t *e)
{
    const bars_t *b = lv_event_get_user_data(e);
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &b->img;
    d.recolor = b->color; /* the colour of an A8 image */
    lv_area_t a;
    lv_obj_get_coords(b->obj, &a);
    lv_draw_image(lv_event_get_layer(e), &d, &a);
}

/* A bar pair `h` px tall at x, y (CHART_W wide), in `color`. */
static void bars_create(bars_t *b, int32_t x, int32_t y, int32_t h, lv_color_t color)
{
    b->px = heap_caps_calloc(CHART_W, h, MALLOC_CAP_SPIRAM);
    assert(b->px != NULL);
    b->img = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = CHART_W, .h = h, .stride = CHART_W },
        .data = b->px,
        .data_size = (uint32_t)CHART_W * h,
    };
    b->color = color;
    b->obj = lv_obj_create(s_detail_root);
    lv_obj_remove_style_all(b->obj);
    lv_obj_set_pos(b->obj, x, y);
    lv_obj_set_size(b->obj, CHART_W, h);
    lv_obj_clear_flag(b->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b->obj, bars_draw_cb, LV_EVENT_DRAW_MAIN, b);
}

/* Draw `n` bar pairs into `b`: front[i] opaque in front of back[i], each
 * value/axis_max of `full_h` px tall (the image may be shorter, cutting off
 * what could never be reached anyway). Adapter lock held. */
static void bars_render(bars_t *b, int n, const int32_t *front, const int32_t *back, int32_t axis_max,
                        int32_t full_h)
{
    const int32_t h = b->img.header.h;
    memset(b->px, 0, (size_t)CHART_W * h);
    if (n <= 0 || axis_max <= 0) {
        lv_obj_invalidate(b->obj);
        return;
    }
    /* Like lv_chart's bars: n slots across, a bar in each with a gap. */
    const float slot = (float)(CHART_W - 8) / n;
    const int32_t w = slot > 3.0f ? (int32_t)(slot * 0.7f + 0.5f) : 1;
    for (int i = 0; i < n; i++) {
        const int32_t x0 = 4 + (int32_t)(i * slot + (slot - w) / 2);
        int32_t hf = (front[i] == LV_CHART_POINT_NONE) ? 0 : front[i] * full_h / axis_max;
        int32_t hb = (back[i] == LV_CHART_POINT_NONE) ? 0 : back[i] * full_h / axis_max;
        hf = hf > h ? h : hf;
        hb = hb > h ? h : hb;
        for (int32_t r = 0; r < h; r++) {
            const uint8_t a = (r < hf) ? 255 : (r < hb) ? BARS_BACK_OPA : 0;
            if (a == 0) {
                break;
            }
            memset(b->px + (size_t)(h - 1 - r) * CHART_W + x0, a, (size_t)w);
        }
    }
    lv_obj_invalidate(b->obj);
}
static EXT_RAM_BSS_ATTR lv_point_precise_t s_temp_line_points[YR_FORECAST_MAX_POINTS];
/* The temperature each s_temp_line_points entry was plotted from, and the
 * chart y of 0 degrees C, so temp_line_draw_cb can colour the sub-zero parts
 * of the line separately. */
static EXT_RAM_BSS_ATTR float s_temp_line_values[YR_FORECAST_MAX_POINTS];
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

/* The overview table: a title, a header row of clock hours
 * (filled in each refresh), then one row per location with a name cell and
 * OV_COLS cells of {weather icon, temperature, precipitation}. Always called;
 * the row loop below is simply empty when no location shows weather. */
lv_obj_t *overview_build(lv_obj_t *screen)
{
    lv_obj_t *root = s_overview_root = screen_root_create(screen);
    /* Centre text in every overview cell by inheritance - avoids a per-label
     * style property on ~30 widgets, which matters for internal DRAM. */
    lv_obj_set_style_text_align(root, LV_TEXT_ALIGN_CENTER, 0);

    s_ov_title = lv_label_create(root);
    lv_obj_set_style_text_font(s_ov_title, g_font_large, 0);
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
        lv_label_set_text(s_ov_name[i], g_cfg->locations[s_wx_loc[i]].name);

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

    return root;
}

static void temp_segment(lv_layer_t *layer, float x1, float y1, float x2, float y2,
                         lv_color_t color)
{
    int w = TEMP_LINE_WIDTH;
    if (!draw_visible(layer, (int)(x1 < x2 ? x1 : x2) - w, (int)(y1 < y2 ? y1 : y2) - w,
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

/* Centers label horizontally on chart_x (absolute) and places it either
 * above or below chart_y (absolute), clamped to stay within the chart's
 * horizontal bounds. */
static int32_t place_marker_label(lv_obj_t *label, int32_t chart_x, int32_t chart_y, bool above)
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
    return x;
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
 * each labelled with its range, e.g. "0.5-2.3 mm", above the max (back)
 * bar, which is always at least as tall as the min. */
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
 * to miss. All on one row, so six hours apart isn't always room enough
 * (wide text, the 10-minute nowcast stretching the first hours, labels
 * pushed in from the chart's edge): a label that would come within
 * WIND_MARKER_GAP px of one already placed - a stronger peak, as they are
 * picked strongest first - is left out. */
static void place_wind_markers(const yr_forecast_t *fc)
{
    int gust_idx[WIND_MARKER_POOL];
    int n = pick_top_peaks(fc, pt_wind_gust, WIND_MARKER_POOL, gust_idx);

    int used = 0;
    int32_t left[WIND_MARKER_POOL], right[WIND_MARKER_POOL]; /* of those placed */
    for (int k = 0; k < n; k++) {
        int m = gust_idx[k];
        int32_t x = (fc->point_count > 1) ? (int32_t)m * (CHART_W - 1) / (fc->point_count - 1) : 0;
        lv_obj_t *label = s_wind_markers[used];
        lv_label_set_text_fmt(label, "%.0f (%.0f) m/s", (double)fc->points[m].wind_speed_ms,
                              (double)fc->points[m].wind_speed_of_gust_ms);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        const int32_t x1 = place_marker_label(label, CHART_X + x, WIND_CHART_Y, false);
        const int32_t x2 = x1 + lv_obj_get_width(label);
        bool clash = false;
        for (int j = 0; j < used; j++) {
            clash |= x1 < right[j] + WIND_MARKER_GAP && left[j] < x2 + WIND_MARKER_GAP;
        }
        if (!clash) {
            left[used] = x1;
            right[used] = x2;
            used++;
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

/* Draw forecast `fc`, fetched at wall-clock `fetched` (0 if the clock
 * wasn't synced): "Oppdatert kl." is when the device got it, not when MET
 * issued it, and turns orange once it's WX_STALE_S old - the chart itself
 * looks the same however old the data is. Adapter lock held. */
/* Show the sun line only where it fits: not with an alert banner, and not
 * over a long location name (adapter lock held). */
static void sun_label_fit(void)
{
    if (lv_label_get_text(s_sun_label)[0] == '\0' || !lv_obj_has_flag(s_alert_label, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_sun_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_update_layout(s_detail_root);
    lv_obj_align_to(s_sun_label, s_updated_label, LV_ALIGN_OUT_LEFT_MID, -28, 0);
    lv_obj_update_layout(s_sun_label);
    const bool fits = lv_obj_get_x(s_sun_label) > lv_obj_get_x(s_location_label) +
                                                     lv_obj_get_width(s_location_label) + 24;
    if (fits) {
        lv_obj_clear_flag(s_sun_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sun_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Today's sun times for location `loc`, and the night hours shaded across
 * the charts, whose x axis runs from fc's first point to its last (adapter
 * lock held). */
/* "Nedbør om 25 min", "Opphold om 10 min", ... from location `loc`'s last
 * nowcast, if recent and it foresees a change; else false. */
static bool rain_text(int loc, time_t now, char *out, size_t out_len)
{
    if (!stamp_fresh(&s_rain_stamp[loc], RAIN_NOTE_MAX_MS) || now <= PLAUSIBLE_EPOCH_S) {
        return false;
    }
    /* Whole 5 minutes, rounded up: the nowcast's own steps. */
    const int64_t left = s_rain_at[loc] - now;
    const int min = (int)((left + 299) / 300 * 5);
    switch (s_rain[loc]) {
    case YR_RAIN_STARTS:
        if (left <= 0) {
            return false; /* started since; the next nowcast says more */
        }
        snprintf(out, out_len, "Nedb\xC3\xB8r om %d min", min);
        return true;
    case YR_RAIN_STOPS:
        if (left <= 0) {
            return false;
        }
        snprintf(out, out_len, "Opphold om %d min", min);
        return true;
    case YR_RAIN_ONGOING:
        snprintf(out, out_len, "Nedb\xC3\xB8r den neste timen");
        return true;
    default:
        return false;
    }
}

static void sun_update(const yr_forecast_t *fc, int loc)
{
    const double lat = atof(g_cfg->locations[loc].lat);
    const double lon = atof(g_cfg->locations[loc].lon);
    const time_t now = time(NULL);

    /* Precipitation starting or stopping soon says more than the sun times,
     * so it takes their place. */
    char text[48] = "";
    const bool rain = rain_text(loc, now, text, sizeof(text));
    lv_obj_set_style_text_color(s_sun_label, rain ? lv_palette_main(LV_PALETTE_BLUE) : s_sun_colour, 0);
    if (!rain && now > PLAUSIBLE_EPOCH_S) {
        time_t rise, set;
        switch (sun_times(lat, lon, now, &rise, &set)) {
        case SUN_UP_ALL_DAY:
            snprintf(text, sizeof(text), "Midnattssol");
            break;
        case SUN_DOWN_ALL_DAY:
            snprintf(text, sizeof(text), "M\xC3\xB8rketid");
            break;
        default: {
            struct tm r, s;
            localtime_r(&rise, &r);
            localtime_r(&set, &s);
            if (rise != 0 && set != 0) {
                snprintf(text, sizeof(text), "Sol %02d:%02d\xE2\x80\x93%02d:%02d", r.tm_hour, r.tm_min, s.tm_hour,
                         s.tm_min);
            } else if (rise != 0) {
                snprintf(text, sizeof(text), "Sol opp %02d:%02d", r.tm_hour, r.tm_min);
            } else {
                snprintf(text, sizeof(text), "Sol ned %02d:%02d", s.tm_hour, s.tm_min);
            }
            break;
        }
        }
    }
    lv_label_set_text(s_sun_label, text);
    sun_label_fit();

    /* Night: every 5 minutes along the axis, the runs with the sun down. */
    int band = 0;
    const int n = fc->point_count;
    if (n > 1 && fc->points[0].epoch_utc > PLAUSIBLE_EPOCH_S) {
        const time_t t0 = (time_t)fc->points[0].epoch_utc, t1 = (time_t)fc->points[n - 1].epoch_utc;
        time_t start = 0;
        for (time_t t = t0; band < NIGHT_BANDS; t += 300) {
            const bool end = t >= t1;
            const time_t at = end ? t1 : t;
            const bool down = !end && sun_elevation_deg(lat, lon, at) <= SUN_DOWN_DEG;
            if (down && start == 0) {
                start = at;
            } else if (!down && start != 0) {
                const int x0 = (int)((start - t0) * (CHART_W - 1) / (t1 - t0));
                const int x1 = (int)((at - t0) * (CHART_W - 1) / (t1 - t0));
                if (x1 - x0 >= 2) {
                    lv_obj_set_x(s_night_main[band], CHART_X + x0);
                    lv_obj_set_width(s_night_main[band], x1 - x0);
                    lv_obj_set_x(s_night_wind[band], CHART_X + x0);
                    lv_obj_set_width(s_night_wind[band], x1 - x0);
                    lv_obj_clear_flag(s_night_main[band], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_night_wind[band], LV_OBJ_FLAG_HIDDEN);
                    band++;
                }
                start = 0;
            }
            if (end) {
                break;
            }
        }
    }
    for (; band < NIGHT_BANDS; band++) {
        lv_obj_add_flag(s_night_main[band], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_night_wind[band], LV_OBJ_FLAG_HIDDEN);
    }
}

/* A shaded night band over a chart, y..y+h (placed by sun_update). */
static lv_obj_t *night_band(int y, int h, bool dark)
{
    lv_obj_t *b = lv_obj_create(s_detail_root);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, dark ? lv_color_hex(0x5070C0) : lv_color_hex(0x203070), 0);
    lv_obj_set_style_bg_opa(b, dark ? LV_OPA_20 : LV_OPA_10, 0);
    lv_obj_set_pos(b, CHART_X, y);
    lv_obj_set_size(b, 1, h);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

/* Aurora: the runs of hours in location `loc`'s aurora forecast worth
 * looking up for (aurora_good: dark, a good chance, the sky at least partly
 * clear), each a green glow down from the top of the main chart with
 * "Nordlys" on it. Same x axis as sun_update (adapter lock held). */
static void aurora_update(const yr_forecast_t *fc, int loc)
{
    int band = 0;
    int label_end = -1000; /* right edge of the last label shown */
    const int n = fc->point_count;
    const aurora_t *a = s_aurora_valid[loc] ? s_aurora_cache[loc] : NULL;
    if (a != NULL && n > 1 && fc->points[0].epoch_utc > PLAUSIBLE_EPOCH_S) {
        const int64_t t0 = fc->points[0].epoch_utc, t1 = fc->points[n - 1].epoch_utc;
        for (int i = 0; i < a->count && band < AURORA_BANDS; i++) {
            if (!aurora_good(&a->hours[i])) {
                continue;
            }
            int64_t start = a->hours[i].start, end = a->hours[i].end;
            while (i + 1 < a->count && aurora_good(&a->hours[i + 1]) && a->hours[i + 1].start == end) {
                end = a->hours[++i].end;
            }
            start = start < t0 ? t0 : start;
            end = end > t1 ? t1 : end;
            if (end <= start) {
                continue; /* outside the chart */
            }
            const int x0 = (int)((start - t0) * (CHART_W - 1) / (t1 - t0));
            const int x1 = (int)((end - t0) * (CHART_W - 1) / (t1 - t0));
            if (x1 - x0 < 2) {
                continue;
            }
            lv_obj_set_x(s_aurora_band[band], CHART_X + x0);
            lv_obj_set_width(s_aurora_band[band], x1 - x0);
            lv_obj_clear_flag(s_aurora_band[band], LV_OBJ_FLAG_HIDDEN);
            /* One label for runs close together (the same night). */
            lv_obj_t *l = s_aurora_label[band];
            lv_obj_update_layout(l);
            const int w = lv_obj_get_width(l);
            int x = CHART_X + (x0 + x1) / 2 - w / 2;
            x = x < CHART_X + 2 ? CHART_X + 2 : (x > CHART_X + CHART_W - 2 - w ? CHART_X + CHART_W - 2 - w : x);
            if (x >= label_end + 8) {
                lv_obj_set_pos(l, x, CHART_Y + 3);
                lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
                label_end = x + w;
            } else {
                lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
            }
            band++;
        }
    }
    for (; band < AURORA_BANDS; band++) {
        lv_obj_add_flag(s_aurora_band[band], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_aurora_label[band], LV_OBJ_FLAG_HIDDEN);
    }
}

/* The glow of an aurora band: green at the top of the main chart, fading
 * out a little over half way down (placed by aurora_update). */
static lv_obj_t *aurora_band(bool dark)
{
    lv_obj_t *b = lv_obj_create(s_detail_root);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(0x2ECC71), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(b, dark ? LV_OPA_50 : LV_OPA_40, 0);
    lv_obj_set_style_bg_grad_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_stop(b, 150, 0);
    lv_obj_set_pos(b, CHART_X, CHART_Y + 1);
    lv_obj_set_size(b, 1, CHART_H - 2);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

static void update_ui_with_forecast(const yr_forecast_t *fc, time_t fetched, int loc)
{
    const yr_forecast_point_t *now = &fc->points[0];

    const time_t wall = time(NULL);
    if (fetched > 0) {
        struct tm lt;
        localtime_r(&fetched, &lt);
        const bool stale = wall - fetched >= WX_STALE_S;
        lv_label_set_text_fmt(s_updated_label, "%s kl. %02d:%02d", stale ? "Sist oppdatert" : "Oppdatert",
                              lt.tm_hour, lt.tm_min);
        if (stale) {
            lv_obj_set_style_text_color(s_updated_label, lv_palette_main(LV_PALETTE_ORANGE), 0);
        } else {
            lv_obj_remove_local_style_prop(s_updated_label, LV_STYLE_TEXT_COLOR, 0);
        }
    } else {
        lv_label_set_text_fmt(s_updated_label, "Varsel fra kl. %s", fc->updated_hour_minute);
    }

    int temp_min_idx = 0, temp_max_idx = 0;
    float temp_min = now->air_temperature_c;
    float temp_max = now->air_temperature_c;
    float precip_max = 0.0f; /* the high end of the range - the back bars */
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

    /* The min in front, the max behind it (see bars_render). */
    bars_render(&s_precip_bars, fc->point_count, s_precip_chart_data, s_precip_max_chart_data,
                precip_range_max * PRECIP_AXIS_COMPRESSION, CHART_H);

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
     * never clips off the top of its own chart. The wind is in front, the
     * gust behind it. */
    float wind_or_gust_max = (gust_max > wind_max) ? gust_max : wind_max;
    int32_t wind_range_max = round_to_int(wind_or_gust_max * 10.0f) + 20;
    if (wind_range_max < 60) {
        wind_range_max = 60;
    }
    bars_render(&s_wind_bars, fc->point_count, s_wind_chart_data, s_gust_chart_data, wind_range_max, WIND_BARS_H);

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

    sun_update(fc, loc);
    aurora_update(fc, loc);
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
    for (int i = 0; i < g_cfg->location_count; i++) {
        if (s_fc_valid[i]) {
            return true;
        }
    }
    return false;
}

/* Whether the overview is still waiting on its first forecast. False (nothing
 * to wait for) when no location shows weather at all - e.g. a radar-only
 * setup - so the overview never gets stuck on "Henter oversikt..." forever. */
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
        sun_label_fit();
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
    sun_label_fit();
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

    int64_t now_epoch = 0;
    for (int i = 0; i < g_cfg->location_count; i++) {
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
        lv_label_set_text(s_ov_name[row], g_cfg->locations[i].name);

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

    /* Not on the stack: a forecast is ~7.6 KB, nearly all of the weather
     * task's 8 KB (it overflowed once the daily summaries were added). Only
     * the weather task calls this. */
    static EXT_RAM_BSS_ATTR yr_forecast_t out;
    out = *src; /* carries over valid / updated_hour_minute / etc. */
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

void weather_init(void)
{
    for (int i = 0; i < g_cfg->location_count; i++) {
        if (g_cfg->show[i] & APP_SHOW_WEATHER) {
            s_wx_loc[s_weather_count++] = (uint8_t)i; /* an overview row */
        }
        if (g_cfg->show[i] & (APP_SHOW_WEATHER | APP_SHOW_WEEK)) {
            s_fc_cache[i] = heap_caps_malloc(sizeof(yr_forecast_t), MALLOC_CAP_SPIRAM);
            s_alert_cache[i] = heap_caps_malloc(sizeof(met_alerts_t), MALLOC_CAP_SPIRAM);
            s_wx_shown[i] = heap_caps_malloc(sizeof(yr_forecast_t), MALLOC_CAP_SPIRAM);
            assert(s_fc_cache[i] != NULL && s_alert_cache[i] != NULL && s_wx_shown[i] != NULL);
        }
        if (g_cfg->show[i] & APP_SHOW_WEATHER) {
            s_aurora_cache[i] = heap_caps_malloc(sizeof(aurora_t), MALLOC_CAP_SPIRAM);
            assert(s_aurora_cache[i] != NULL);
        }
    }
    s_scratch = heap_caps_malloc(sizeof(*s_scratch), MALLOC_CAP_SPIRAM);
    s_aurora_scratch = heap_caps_malloc(sizeof(*s_aurora_scratch), MALLOC_CAP_SPIRAM);
    assert(s_aurora_scratch != NULL);
    s_merged = heap_caps_malloc(sizeof(*s_merged), MALLOC_CAP_SPIRAM);
    s_resampled = heap_caps_malloc(sizeof(*s_resampled), MALLOC_CAP_SPIRAM);
    s_nowcast = heap_caps_malloc(sizeof(*s_nowcast), MALLOC_CAP_SPIRAM);
    s_alert_scratch = heap_caps_malloc(sizeof(*s_alert_scratch), MALLOC_CAP_SPIRAM);
    assert(s_scratch != NULL && s_merged != NULL && s_resampled != NULL && s_nowcast != NULL &&
           s_alert_scratch != NULL);
}

lv_obj_t *weather_build(lv_obj_t *screen)
{
    const bool dark = (g_cfg->theme == APP_THEME_DARK);
    s_detail_root = screen_root_create(screen);

    s_location_label = lv_label_create(s_detail_root);
    lv_obj_set_style_text_font(s_location_label, g_font_large, 0);
    lv_obj_set_pos(s_location_label, 12, 4);
    lv_label_set_text(s_location_label, "");

    s_updated_label = lv_label_create(s_detail_root);
    lv_obj_align(s_updated_label, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_label_set_text(s_updated_label, "");

    s_sun_label = lv_label_create(s_detail_root);
    s_sun_colour = dark ? lv_color_hex(0xB0B0B0) : lv_color_hex(0x606060);
    lv_obj_set_style_text_color(s_sun_label, s_sun_colour, 0);
    lv_label_set_text(s_sun_label, "");
    lv_obj_add_flag(s_sun_label, LV_OBJ_FLAG_HIDDEN);

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

    /* The frame (background, border, gridlines) of the precipitation and
     * temperature area: a chart with no series of its own. The bars are
     * s_precip_bars on top of it (see bars_t), and the temperature line on
     * top of those. */
    s_precip_frame = lv_chart_create(s_detail_root);
    lv_obj_set_pos(s_precip_frame, CHART_X, CHART_Y);
    lv_obj_set_size(s_precip_frame, CHART_W, CHART_H);
    lv_chart_set_type(s_precip_frame, LV_CHART_TYPE_NONE);
    lv_chart_set_div_line_count(s_precip_frame, 4, NUM_HOUR_LABELS - 1);
    for (int i = 0; i < NIGHT_BANDS; i++) {
        s_night_main[i] = night_band(CHART_Y + 1, CHART_H - 2, dark);
    }
    for (int i = 0; i < AURORA_BANDS; i++) {
        s_aurora_band[i] = aurora_band(dark);
    }
    /* Bars only ever fill the bottom 1/PRECIP_AXIS_COMPRESSION of the area. */
    bars_create(&s_precip_bars, CHART_X, CHART_Y + CHART_H - BARS_INSET - PRECIP_BARS_H, PRECIP_BARS_H,
                lv_palette_main(LV_PALETTE_BLUE));

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

    for (int i = 0; i < AURORA_BANDS; i++) {
        s_aurora_label[i] = lv_label_create(s_detail_root);
        lv_obj_set_style_text_color(s_aurora_label[i], dark ? lv_color_hex(0x7DF0A8) : lv_color_hex(0x168A47), 0);
        lv_label_set_text(s_aurora_label[i], "Nordlys");
        lv_obj_add_flag(s_aurora_label[i], LV_OBJ_FLAG_HIDDEN);
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

    /* The wind/gust area (m/s), same x-scale as the main chart above: a
     * transparent frame for the border and gridlines, and s_wind_bars. */
    s_wind_frame = lv_chart_create(s_detail_root);
    lv_obj_set_pos(s_wind_frame, CHART_X, WIND_CHART_Y);
    lv_obj_set_size(s_wind_frame, CHART_W, WIND_CHART_H);
    lv_obj_set_style_bg_opa(s_wind_frame, LV_OPA_TRANSP, 0);
    lv_chart_set_type(s_wind_frame, LV_CHART_TYPE_NONE);
    lv_chart_set_div_line_count(s_wind_frame, 2, NUM_HOUR_LABELS - 1);
    for (int i = 0; i < NIGHT_BANDS; i++) {
        s_night_wind[i] = night_band(WIND_CHART_Y + 1, WIND_CHART_H - 2, dark);
    }
    bars_create(&s_wind_bars, CHART_X, WIND_CHART_Y + BARS_INSET, WIND_BARS_H, lv_palette_main(LV_PALETTE_TEAL));

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

    return s_detail_root;
}

void weather_enter(int loc)
{
    lv_label_set_text(s_location_label, g_cfg->locations[loc].name);
    /* Unlike the forecast, the alert cache carries over as-is from whatever
     * this location's last fetch found. */
    update_alert_banner(loc);
    if (s_wx_shown[loc] != NULL && stamp_fresh(&s_wx_shown_at[loc], WX_CACHE_MAX_MS)) {
        update_ui_with_forecast(s_wx_shown[loc], s_wx_shown_at[loc].when, loc);
        lv_label_set_text(g_status_label, "");
    } else {
        /* Never another location's chart under this one's name: hidden until
         * the forecast and nowcast for it land. */
        lv_obj_add_flag(s_detail_root, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(g_status_label, "Henter v\xC3\xA6rvarsel for %s...", g_cfg->locations[loc].name);
    }
}

void overview_enter(void)
{
    update_overview();
    lv_label_set_text(g_status_label, overview_loading() ? "Henter oversikt..." : "");
}

/* Refresh any stale or missing location forecast and alerts, of locations
 * k0 .. k1 - 1 counted on from the selected one (0 is it): the weather
 * screen asks for its own first and draws it before the rest, which keep
 * the overview warm. N <= 5 and the cadence is 10 min, so this is a fetch or two
 * per wake at most. Only on the weather screens and the overview: the radar
 * and departure board poll often and are the only thing that should use the
 * network there; whatever went stale is refreshed when a weather screen or
 * the overview is next shown. */
static void refresh_caches(bool overview, int sel, bool refetch_sel, int for_view, int k0, int k1)
{
    TickType_t now_tk = xTaskGetTickCount(); /* unsigned - wrap-safe deltas */
    k1 = k1 < g_cfg->location_count ? k1 : g_cfg->location_count;
    for (int k = k0; k < k1; k++) {
        if (g_view_index != for_view) {
            return; /* view changed mid-scan */
        }
        int i = (sel + k) % g_cfg->location_count;
        wd_weather_beat();
        if (!(g_cfg->show[i] & (APP_SHOW_WEATHER | APP_SHOW_WEEK))) {
            continue; /* radar-only location: no forecast needed */
        }
        bool stale = !s_fc_valid[i] || (i == sel && refetch_sel) ||
                     (now_tk - s_fc_tk[i]) >= pdMS_TO_TICKS(WEATHER_REFRESH_INTERVAL_MS);
        bool alert_stale = !s_alert_valid[i] ||
                           (now_tk - s_alert_tk[i]) >= pdMS_TO_TICKS(ALERT_REFRESH_INTERVAL_MS);
        if (!stale && !alert_stale) {
            continue;
        }

        double lat = atof(g_cfg->locations[i].lat);
        double lon = atof(g_cfg->locations[i].lon);

        if (stale) {
            if (!s_fc_valid[i]) {
                memset(&s_fc_http[i], 0, sizeof(s_fc_http[i]));
            }
            esp_err_t err = yr_client_fetch_forecast(lat, lon, s_scratch, &s_fc_http[i], i == sel && refetch_sel);
            if (err == ESP_OK || err == HTTP_NOT_MODIFIED) {
                diag_ok(DIAG_FORECAST);
            } else {
                diag_fail(DIAG_FORECAST, err);
            }
            if (err == HTTP_NOT_MODIFIED) {
                ESP_LOGI(TAG, "Forecast[%d] %s: unchanged (%s)", i, g_cfg->locations[i].name,
                         (i == sel && refetch_sel) ? "asked MET" : "not expired");
                /* The one held is still MET's latest: count it as fetched. */
                if (esp_lv_adapter_lock(-1) == ESP_OK) {
                    s_fc_tk[i] = now_tk;
                    s_fc_when[i] = (time(NULL) > PLAUSIBLE_EPOCH_S) ? time(NULL) : 0;
                    esp_lv_adapter_unlock();
                }
            } else if (err == ESP_OK &&
                s_scratch->valid && s_scratch->point_count > 0 && esp_lv_adapter_lock(-1) == ESP_OK) {
                /* Under the lock: the overview may be drawn from a tap. */
                *s_fc_cache[i] = *s_scratch;
                s_fc_valid[i] = true;
                s_fc_tk[i] = now_tk;
                s_fc_when[i] = (time(NULL) > PLAUSIBLE_EPOCH_S) ? time(NULL) : 0;
                esp_lv_adapter_unlock();
                ESP_LOGI(TAG, "Forecast[%d] %s: %d pts kl. %s (free int %u)",
                         i, g_cfg->locations[i].name, s_fc_cache[i]->point_count,
                         s_fc_cache[i]->updated_hour_minute,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            } else {
                ESP_LOGW(TAG, "Forecast[%d] %s failed; keeping previous", i, g_cfg->locations[i].name);
            }
        }

        if (alert_stale) {
            if (!s_alert_valid[i]) {
                memset(&s_alert_http[i], 0, sizeof(s_alert_http[i]));
            }
            esp_err_t err = met_alerts_client_fetch(lat, lon, s_alert_scratch, &s_alert_http[i]);
            if (err == ESP_OK || err == HTTP_NOT_MODIFIED) {
                diag_ok(DIAG_ALERTS);
            } else {
                diag_fail(DIAG_ALERTS, err);
            }
            if (err == HTTP_NOT_MODIFIED) {
                s_alert_tk[i] = now_tk;
            } else if (err == ESP_OK && s_alert_scratch->valid &&
                esp_lv_adapter_lock(-1) == ESP_OK) {
                *s_alert_cache[i] = *s_alert_scratch;
                s_alert_valid[i] = true;
                s_alert_tk[i] = now_tk;
                esp_lv_adapter_unlock();
                if (s_alert_scratch->count > 0) {
                    ESP_LOGI(TAG, "Alerts[%d] %s: %d active", i, g_cfg->locations[i].name,
                             s_alert_scratch->count);
                }
            } else {
                ESP_LOGW(TAG, "Alerts[%d] %s failed; keeping previous", i, g_cfg->locations[i].name);
            }
        }

        /* Fill the overview row-by-row (forecast + alert badge) as each
         * location lands, and keep the selected detail screen's banner
         * current the moment its own alert fetch lands. */
        if (lock_for_view(for_view)) {
            if (overview) {
                overview_enter();
            } else if (i == sel) {
                update_alert_banner(sel);
            }
            esp_lv_adapter_unlock();
        }
    }
}

/* The selected location's detail screen: its forecast with a fresh nowcast
 * spliced in. */
static void show_selected(int sel, int for_view)
{
    /* Keeps the banner in sync with whatever the fetch loop last landed for
     * this location, even on a pass that finds nothing else to do (e.g. the
     * forecast is still fresh). */
    if (lock_for_view(for_view)) {
        update_alert_banner(sel);
        esp_lv_adapter_unlock();
    }

    /* Nowcast refreshes every 5 min upstream - fetch it every cycle. */
    double lat = atof(g_cfg->locations[sel].lat);
    double lon = atof(g_cfg->locations[sel].lon);
    if (sel != s_nc_loc || !s_nc_held) {
        memset(&s_nc_http, 0, sizeof(s_nc_http)); /* holding another location's */
    }
    esp_err_t nc_err = yr_client_fetch_nowcast(lat, lon, s_nowcast, &s_nc_http, false);
    if (nc_err == HTTP_NOT_MODIFIED) {
        nc_err = ESP_OK; /* s_nowcast is still sel's latest */
    } else {
        s_nc_loc = sel;
        s_nc_held = (nc_err == ESP_OK);
    }
    if (nc_err == ESP_OK) {
        diag_ok(DIAG_NOWCAST);
        int64_t at;
        s_rain[sel] = yr_rain_change(s_nowcast, time(NULL), &at);
        s_rain_at[sel] = at;
        stamp_now(&s_rain_stamp[sel]);
    } else {
        diag_fail(DIAG_NOWCAST, nc_err);
    }
    bool nc_ok = (nc_err == ESP_OK &&
                  s_nowcast->valid && s_nowcast->radar_ok && s_nowcast->point_count > 0);

    if (!(s_fc_valid[sel] && s_fc_cache[sel]->point_count > 0)) {
        if (lock_for_view(for_view)) {
            lv_label_set_text(g_status_label, "Kunne ikke hente v\xC3\xA6rvarsel. Pr\xC3\xB8ver igjen...");
            esp_lv_adapter_unlock();
        }
        return;
    }
    const yr_forecast_t *to_render = s_fc_cache[sel];
    if (nc_ok) {
        merge_nowcast(s_merged, s_fc_cache[sel], s_nowcast);
        to_render = s_merged;
        ESP_LOGI(TAG, "Nowcast merged for %s: %d steps -> %d points",
                 g_cfg->locations[sel].name, s_nowcast->point_count, s_merged->point_count);
    }
    if (to_render->point_count == 0) {
        return;
    }
    /* Every chart/label positions by index, so put the points on a uniform
     * time grid first (see resample_uniform_time) - otherwise the
     * nowcast-merged series' densely-sampled first ~2h would visually eat as
     * much of the x-axis as several hours further out. */
    resample_uniform_time(s_resampled, to_render);
    if (lock_for_view(for_view)) {
        lv_label_set_text(g_status_label, "");
        update_ui_with_forecast(s_resampled, s_fc_when[sel], sel);
        lv_obj_clear_flag(s_detail_root, LV_OBJ_FLAG_HIDDEN);
        *s_wx_shown[sel] = *s_resampled;
        stamp_now(&s_wx_shown_at[sel]);
        s_wx_shown_at[sel].when = s_fc_when[sel]; /* the forecast's age, not the drawing's */
        esp_lv_adapter_unlock();
    }
}

const yr_forecast_t *weather_forecast(int loc, time_t *fetched)
{
    *fetched = s_fc_when[loc];
    return (s_fc_cache[loc] != NULL && s_fc_valid[loc]) ? s_fc_cache[loc] : NULL;
}

void weather_refresh(int loc, int for_view)
{
    refresh_caches(false, loc, false, for_view, 0, APP_CONFIG_MAX_LOCATIONS);
}

/* The aurora forecasts, after the screen is up so they don't hold it back:
 * the selected location's first, its chart marked as soon as it lands
 * (see aurora_update), then the others'. */
static void refresh_aurora(bool overview, int sel, int for_view)
{
    TickType_t now_tk = xTaskGetTickCount();
    for (int k = 0; k < g_cfg->location_count; k++) {
        if (g_view_index != for_view) {
            return;
        }
        int i = (sel + k) % g_cfg->location_count;
        if (s_aurora_cache[i] == NULL ||
            (s_aurora_tried[i] && (now_tk - s_aurora_tk[i]) < pdMS_TO_TICKS(AURORA_REFRESH_INTERVAL_MS))) {
            continue;
        }
        wd_weather_beat();
        double lat = atof(g_cfg->locations[i].lat);
        double lon = atof(g_cfg->locations[i].lon);
        if (!s_aurora_valid[i]) {
            memset(&s_aurora_http[i], 0, sizeof(s_aurora_http[i]));
        }
        esp_err_t err = aurora_client_fetch(lat, lon, s_aurora_scratch, &s_aurora_http[i]);
        s_aurora_tried[i] = true;
        if (err == ESP_OK || err == HTTP_NOT_MODIFIED) {
            diag_ok(DIAG_AURORA);
        } else {
            diag_fail(DIAG_AURORA, err);
        }
        if (err == HTTP_NOT_MODIFIED) {
            s_aurora_tk[i] = now_tk;
        } else if (err == ESP_OK && esp_lv_adapter_lock(-1) == ESP_OK) {
            *s_aurora_cache[i] = *s_aurora_scratch;
            s_aurora_valid[i] = true;
            s_aurora_tk[i] = now_tk;
            esp_lv_adapter_unlock();
            int good = 0;
            for (int h = 0; h < s_aurora_scratch->count; h++) {
                good += aurora_good(&s_aurora_scratch->hours[h]);
            }
            ESP_LOGI(TAG, "Aurora[%d] %s: %d hours, %d worth looking up", i, g_cfg->locations[i].name,
                     s_aurora_scratch->count, good);
        } else {
            /* Yr's website API may change: try again in an hour. */
            s_aurora_tk[i] = now_tk;
            ESP_LOGW(TAG, "Aurora[%d] %s failed; keeping previous", i, g_cfg->locations[i].name);
        }
        if (!overview && i == sel && s_wx_shown_at[sel].valid && lock_for_view(for_view)) {
            aurora_update(s_wx_shown[sel], sel);
            esp_lv_adapter_unlock();
        }
    }
}

uint32_t weather_poll(bool overview, int sel, bool refetch_sel, int for_view)
{
    /* The overview needs every location; a weather screen only its own
     * before it can be drawn. */
    refresh_caches(overview, sel, refetch_sel, for_view, 0, overview ? APP_CONFIG_MAX_LOCATIONS : 1);
    if (g_view_index != for_view) {
        return 0;
    }
    if (overview) {
        if (lock_for_view(for_view)) {
            overview_enter();
            esp_lv_adapter_unlock();
        }
    } else {
        show_selected(sel, for_view);
    }
    refresh_aurora(overview, sel, for_view);
    if (!overview) {
        refresh_caches(false, sel, false, for_view, 1, APP_CONFIG_MAX_LOCATIONS);
    }
    if (g_view_index != for_view) {
        return 0;
    }
    /* Poll on the nowcast cadence once something is on screen; retry fast
     * while still waiting for the first data. */
    const bool ready = overview ? !overview_loading() : s_fc_valid[sel];
    return ready ? NOWCAST_REFRESH_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS;
}

