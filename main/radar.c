/* The radar screen, shared by three kinds of stop: aircraft (adsb_client),
 * ship traffic (ais_client) and rain (rain_client), each around one
 * location and over its coastline (coast_render).
 *
 * Aircraft radar screen (one per location that has it ticked in the setup
 * portal): a sonar-style plot on the left, a table of the nearest aircraft on
 * the right. Everything is drawn in one custom draw callback rather than as
 * LVGL objects - an object per aircraft/label would cost scarce internal DRAM. */

#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "adsb_client.h"
#include "ais_client.h"
#include "app.h"
#include "diag.h"
#include "draw.h"
#include "radar.h"
#include "rain_client.h"
#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "radar";
/* The plot as far left as its "V" label allows, to give the table room for
 * speed and distance at larger text sizes. */
#define RADAR_CX            214
#define RADAR_CY            262
#define RADAR_R             176   /* outer ring radius, px */
#define RADAR_LIST_X        430
#define RADAR_LIST_Y        78
#define RADAR_LIST_ROW_H    26
#define RADAR_LIST_ROWS     14
#define RADAR_LIST_R        (BOARD_LCD_H_RES - 6) /* right edge of the table */
#define RADAR_COL_GAP       8
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
/* Rain radar (same screen again): MET makes a new image every 5 minutes.
 * Retried sooner while there is none yet. The screen loops through the last
 * hour of them, a frame every RAIN_ANIM_MS, resting on the latest for
 * RAIN_HOLD_TICKS more. */
#define RAIN_POLL_MS        (5 * 60 * 1000)
#define RAIN_RETRY_MS       30000
#define RAIN_STEP_S         300
#define RAIN_FRAMES         (60 * 60 / RAIN_STEP_S + 1)
#define RAIN_ANIM_MS        500
#define RAIN_HOLD_TICKS     4
#define RAIN_BUDGET         (1024 * 1024) /* bytes for all the frames at most */
/* PSRAM left free after the frames are allocated: the watchdog restarts
 * below 300 KB, and a forecast parse needs ~350 KB for a moment. With
 * less, the frames get coarser cells instead (see rain_prepare). */
#define RAIN_RESERVE        (512 * 1024)
/* Aircraft radar / ship traffic / rain radar colours, one set per theme.
 * ship[] is per ais_category_t, rain[] per rain level (1..RAIN_LEVELS). */
typedef struct {
    uint32_t bg, disc, ring, txt, dim, plane, vec, apt, coast, water;
    uint32_t ship[AIS_CAT_COUNT];
    uint32_t rain[RAIN_LEVELS];
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
    .rain = { 0x3E7F35, 0x5DB33B, 0xE8D234, 0xF08A24, 0xE0352A },
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
    .rain = { 0xA6DB8E, 0x5DB33B, 0xF2D22E, 0xF08A24, 0xD7301F },
};
static const radar_palette_t *s_rp = &RADAR_DARK; /* set from the theme in radar_build */

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
static bool s_rain_mode;

/* Coastline under the aircraft or ships (see coast_render): an A8 coverage
 * image of the radar disc, drawn in s_rp->coast, over an A8 mask of the
 * water, drawn in s_rp->water. They show the disc around s_coast_at
 * (a location, or a location's centre for ships) at s_coast_km; s_coast_valid gates drawing them. */
#define COAST_D  (2 * RADAR_R + 1)
static uint8_t *s_coast_px;         /* PSRAM, COAST_D x COAST_D */
static uint8_t *s_water_px;         /* PSRAM, COAST_D x COAST_D */
static lv_image_dsc_t s_coast_img;
static lv_image_dsc_t s_water_img;
/* Coastline segment middles with the normal to their water side, noted by
 * coast_seed for coast_fill_water: the middle in 1/16 px, the unit normal
 * times 127 - 6 bytes a seed rather than 16, as there may be tens of
 * thousands. */
#define SEED_POS 16.0f
#define SEED_DIR 127.0f
typedef struct {
    int16_t x, y;
    int8_t nx, ny;
} water_seed_t;
#define WATER_SEEDS_MAX 32768
static water_seed_t *s_water_seeds; /* PSRAM, WATER_SEEDS_MAX */
static int s_water_n_seeds;
static bool s_coast_valid;
static bool s_coast_retry;           /* the one drawn is incomplete: draw it again */
/* Rain over the disc: the last hour of radar images, each kept as the
 * s_rain_crop of MET's rain levels under the disc (see rain_poll), and the
 * one on show drawn into an ARGB8888 image the size of the coastline one
 * (see rain_render). Frame position p (0 = an hour back, RAIN_FRAMES - 1 =
 * the latest) is the image taken at s_rain_latest - (RAIN_FRAMES - 1 - p) *
 * RAIN_STEP_S; s_rain_ftime says which slot holds which (RAIN_EMPTY if
 * none). Written by the weather task with the adapter lock held, played by
 * rain_anim_timer_cb. */
#define RAIN_EMPTY  ((time_t)-1)
#define RAIN_GRID   20
#define RAIN_GRID_N (COAST_D / RAIN_GRID + 2)
static uint8_t *s_rain_frame[RAIN_FRAMES]; /* PSRAM, s_rain_cells each */
static time_t s_rain_ftime[RAIN_FRAMES];
static uint8_t *s_rain_spare;       /* PSRAM; rain_poll fetches here, then swaps it in */
static size_t s_rain_cells;         /* what the frame buffers were allocated for */
static const rain_area_t *s_rain_area;
static rain_crop_t s_rain_crop;
/* Where every RAIN_GRID-th disc pixel falls in the crop, in cells. */
static EXT_RAM_BSS_ATTR float s_rain_gx[RAIN_GRID_N][RAIN_GRID_N];
static EXT_RAM_BSS_ATTR float s_rain_gy[RAIN_GRID_N][RAIN_GRID_N];
static float s_rain_col[RAIN_LEVELS + 1][4]; /* premultiplied colour per level; 0 is clear */
static time_t s_rain_latest;
static uint32_t *s_rain_px;         /* PSRAM, COAST_D x COAST_D */
static lv_image_dsc_t s_rain_img;
static bool s_rain_valid;           /* s_rain_px holds a frame */
static int s_rain_pos = -1;         /* frame position on show */
static int s_rain_hold;             /* ticks left resting on the latest */
static bool s_rain_loading;         /* the hour's frames are being fetched: the latest held still */
static time_t s_rain_time;          /* when the frame on show was taken */
static const app_location_t *s_coast_at;
/* The location and range the rain frames are held for (see rain_prepare). */
static int s_rain_prep_loc = -1, s_rain_prep_range;
/* Frames older than this aren't shown again on coming back to the screen. */
#define RAIN_KEEP_S         (15 * 60)
static int s_coast_km;
static int s_radar_loc = -1;        /* location the radar screen is set to */
static const app_location_t *s_radar_at; /* where its disc is centred (in g_cfg) */
#define RADAR_CACHE_MAX_MS  30000              /* dead-reckoned from here on */
#define SHIP_CACHE_MAX_MS   (5 * 60 * 1000)
static adsb_result_t *s_adsb_cache[APP_CONFIG_MAX_LOCATIONS];
static fetch_stamp_t s_adsb_at[APP_CONFIG_MAX_LOCATIONS];
static ais_result_t *s_ais_cache[APP_CONFIG_MAX_LOCATIONS];
static fetch_stamp_t s_ais_at[APP_CONFIG_MAX_LOCATIONS];
/* PSRAM; each fetch lands here first, so a failed one keeps the last. */
static adsb_result_t *s_adsb_scratch;
static ais_result_t *s_ais_scratch;

/* The radar screen now shows `loc`, centred on `at`, at `range_km` (adapter
 * lock held): stop drawing a coastline image made for anything else until
 * coast_render redoes it. */
static void coast_mark(int loc, const app_location_t *at, int range_km)
{
    s_radar_loc = loc;
    s_radar_at = at;
    if (at != s_coast_at || range_km != s_coast_km) {
        s_coast_valid = false;
    }
}

/* A circle centred on the radar: an outline, optionally filled. */

/* --------------------------------------------------------------------------
 * Aircraft radar
 *
 * The plot and table are painted straight into the draw layer from one
 * LV_EVENT_DRAW_MAIN handler, with draw.c's helpers.
 * ------------------------------------------------------------------------ */

static void radar_circle(lv_layer_t *layer, int r, int border_w, lv_color_t border,
                         bool fill, lv_color_t fill_color)
{
    lv_area_t a = { RADAR_CX - r, RADAR_CY - r, RADAR_CX + r, RADAR_CY + r };
    if (!draw_area_visible(layer, &a)) {
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
            draw_line(layer,
                       RADAR_CX + (int)lroundf(w->x1 * px_per_km), RADAR_CY - (int)lroundf(w->y1 * px_per_km),
                       RADAR_CX + (int)lroundf(w->x2 * px_per_km), RADAR_CY - (int)lroundf(w->y2 * px_per_km),
                       2, col);
        }
        if (longest < 8.0f) {
            draw_dot(layer, cx, cy, 3, col);
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
        lv_text_get_size(&sz, a->label, g_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

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
        draw_text(layer, a->label, lab.x1, lab.y1, sz.x + 2, LV_TEXT_ALIGN_LEFT, col);
    }
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

/* Table columns beside the radar (x and width in px), measured from the
 * body font at start-up (radar_layout_tables): the text size is a setting,
 * so fixed widths would let a larger font wrap or run cells together. */
typedef struct {
    int x, w; /* w == 0: column left out */
} radar_col_t;
enum { AC_CALL, AC_TYPE, AC_ALT, AC_GS, AC_DIST, AC_COLS };
enum { SH_NAME, SH_TYPE, SH_KN, SH_DIST, SH_COLS };
static radar_col_t s_ac_col[AC_COLS];
/* The route column shows only the destination ("Til": "BGO") when the whole
 * route ("OSL-BGO") doesn't fit. */
static bool s_route_short;
static int s_col_gap = RADAR_COL_GAP;
static radar_col_t s_sh_col[SH_COLS];

/* Place columns 1..n-1 right to left from the table's right edge, each
 * w[i] wide (0 = left out); column 0 gets whatever is left. Returns its
 * width. */
static int radar_layout_cols(radar_col_t *c, const int *w, int n)
{
    int right = RADAR_LIST_R;
    for (int i = n - 1; i >= 1; i--) {
        c[i].w = w[i];
        if (w[i] > 0) {
            c[i].x = right - w[i];
            right = c[i].x - s_col_gap;
        }
    }
    c[0].x = RADAR_LIST_X;
    c[0].w = right - RADAR_LIST_X;
    return c[0].w;
}

static int radar_max_w(const char *const *txt, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        int t = draw_text_w(txt[i]);
        w = t > w ? t : w;
    }
    return w;
}

/* Each column as wide as its header or its widest likely value. The aircraft
 * type is left out if the callsign would otherwise get too narrow for a
 * typical one; names, callsigns and types that still don't fit are shortened
 * with "..". */
static void radar_layout_tables(void)
{
    int ac[AC_COLS] = {
        /* "WWW": the widest any three-letter airport code can be. */
        [AC_TYPE] = radar_max_w((const char *[]){ "Rute", "B77W", "WWW-WWW" }, 3),
        [AC_ALT] = radar_max_w((const char *[]){ "H\xC3\xB8yde", "00.0 km", "88.8 km", "000 m", "888 m" }, 5),
        [AC_GS] = radar_max_w((const char *[]){ "kt", "000", "888" }, 3),
        [AC_DIST] = radar_max_w((const char *[]){ "km", "000", "888" }, 3),
    };
    /* Too narrow for a typical callsign (a large font): the destination
     * only (no type), a little closer together; then without the speed;
     * then without the distance too (the plot's rings show it); failing
     * that, speed and distance back and no route column. */
    const int call_w = draw_text_w("SAS1234");
    if (radar_layout_cols(s_ac_col, ac, AC_COLS) < call_w) {
        const int gs_w = ac[AC_GS], dist_w = ac[AC_DIST];
        s_route_short = true;
        s_col_gap = RADAR_COL_GAP - 2;
        ac[AC_TYPE] = radar_max_w((const char *[]){ "Til", "WWW" }, 2);
        if (radar_layout_cols(s_ac_col, ac, AC_COLS) < call_w) {
            ac[AC_GS] = 0;
        }
        if (radar_layout_cols(s_ac_col, ac, AC_COLS) < call_w) {
            ac[AC_DIST] = 0;
            if (radar_layout_cols(s_ac_col, ac, AC_COLS) < call_w) {
                s_route_short = false;
                s_col_gap = RADAR_COL_GAP;
                ac[AC_TYPE] = 0;
                ac[AC_GS] = gs_w;
                ac[AC_DIST] = dist_w;
                radar_layout_cols(s_ac_col, ac, AC_COLS);
            }
        }
    }

    const int ac_gap = s_col_gap;
    s_col_gap = RADAR_COL_GAP; /* the ship table keeps the usual gap */
    const char *cats[AIS_CAT_COUNT + 1];
    for (int i = 0; i < AIS_CAT_COUNT; i++) {
        cats[i] = ais_category_label(i);
    }
    cats[AIS_CAT_COUNT] = "Type";
    int sh[SH_COLS] = {
        [SH_TYPE] = radar_max_w(cats, AIS_CAT_COUNT + 1),
        [SH_KN] = radar_max_w((const char *[]){ "kn", "00", "88" }, 3),
        [SH_DIST] = radar_max_w((const char *[]){ "km", "000", "888", "8.8" }, 4),
    };
    radar_layout_cols(s_sh_col, sh, SH_COLS);
    s_col_gap = ac_gap;
}

/* One table cell: left-aligned text is shortened to fit; right-aligned
 * (numbers) is drawn in an area exactly its own width against the column's
 * right edge, so it can't wrap onto the next row. Nothing for a left-out
 * column. */
static void radar_cell(lv_layer_t *layer, const radar_col_t *c, const char *txt, int y,
                       lv_text_align_t align, lv_color_t color)
{
    if (c->w <= 0) {
        return;
    }
    if (align == LV_TEXT_ALIGN_LEFT) {
        draw_text_fit(layer, txt, c->x, y, c->w, color);
    } else if (draw_visible(layer, c->x - RADAR_COL_GAP, y, c->x + c->w, y + lv_font_get_line_height(g_font_body))) {
        /* Measured only in the strips it's drawn in - this runs per strip. */
        const int w = draw_text_w(txt) + 1;
        draw_text(layer, txt, c->x + c->w - w, y, w, LV_TEXT_ALIGN_RIGHT, color);
    }
}

/* Ship traffic on the radar grid (already drawn): a hull-shaped marker along
 * each ship's heading with a SHIP_VEC_MIN-minute course vector, a dot for
 * moored / anchored ones, name tags for the nearest (those underway first),
 * and the table. */
static void ships_draw(lv_layer_t *layer, int range, int lh)
{
    const lv_color_t c_ring = lv_color_hex(s_rp->ring);
    const lv_color_t c_txt = lv_color_hex(s_rp->txt);
    const lv_color_t c_dim = lv_color_hex(s_rp->dim);

    draw_line(layer, RADAR_LIST_X - 10, 48, RADAR_LIST_X - 10, 470, 1, c_ring);
    radar_cell(layer, &s_sh_col[SH_NAME], "Navn", 50, LV_TEXT_ALIGN_LEFT, c_dim);
    radar_cell(layer, &s_sh_col[SH_TYPE], "Type", 50, LV_TEXT_ALIGN_LEFT, c_dim);
    radar_cell(layer, &s_sh_col[SH_KN], "kn", 50, LV_TEXT_ALIGN_RIGHT, c_dim);
    radar_cell(layer, &s_sh_col[SH_DIST], "km", 50, LV_TEXT_ALIGN_RIGHT, c_dim);

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
            draw_dot(layer, px, py, 3, col);
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
                draw_line(layer, px, py, px + (int)lroundf(vx * vec_px), py + (int)lroundf(vy * vec_px),
                           1, col);
            }
            draw_triangle(layer, tri, col);
        }
    }

    /* Name tags for up to RADAR_TAGS ships, on the side facing the centre,
     * skipped where they'd overlap an earlier one: first the ships underway,
     * nearest first, then the moored / anchored ones in what room is left.
     * Computed in the same order on every strip so the choice is consistent
     * across the whole frame. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < res->count && n_tags < RADAR_TAGS; i++) {
            const ais_ship_t *s = &res->ship[i];
            if (s->moored != (pass == 1)) {
                continue;
            }
            float sx, sy;
            if (!ship_plot_pos(s, range, since_fetch_s, &sx, &sy)) {
                continue;
            }
            int px = RADAR_CX + (int)lroundf(sx);
            int py = RADAR_CY - (int)lroundf(sy);
            lv_point_t sz;
            lv_text_get_size(&sz, s->name, g_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
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
                draw_text_fit(layer, s->name, tag.x1, tag.y1, w, c_txt);
            }
        }
    }

    if (res->count == 0) {
        char none[48];
        snprintf(none, sizeof(none), "Ingen skip innen %d km", range);
        draw_text(layer, none, RADAR_LIST_X, RADAR_LIST_Y, RADAR_LIST_R - RADAR_LIST_X, LV_TEXT_ALIGN_LEFT, c_txt);
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
        radar_cell(layer, &s_sh_col[SH_NAME], s->name, y, LV_TEXT_ALIGN_LEFT, c_txt);
        radar_cell(layer, &s_sh_col[SH_TYPE], ais_category_label(s->category), y, LV_TEXT_ALIGN_LEFT, col);
        radar_cell(layer, &s_sh_col[SH_KN], kn, y, LV_TEXT_ALIGN_RIGHT, c_txt);
        radar_cell(layer, &s_sh_col[SH_DIST], dist, y, LV_TEXT_ALIGN_RIGHT, c_txt);
    }
    if (res->total > res->count) {
        char more[40];
        snprintf(more, sizeof(more), "Viser %d av %d skip", res->count, res->total);
        draw_text(layer, more, RADAR_LIST_X, RADAR_LIST_Y + RADAR_LIST_ROWS * RADAR_LIST_ROW_H + 4,
                   RADAR_LIST_R - RADAR_LIST_X, LV_TEXT_ALIGN_LEFT, c_dim);
    }
}

/* The slot holding the image taken at `t`, or -1. */
static int rain_slot_for(time_t t)
{
    for (int i = 0; i < RAIN_FRAMES; i++) {
        if (s_rain_ftime[i] != RAIN_EMPTY && s_rain_ftime[i] == t) {
            return i;
        }
    }
    return -1;
}

/* The slot holding frame position `p`, or -1. */
static int rain_slot_at(int p)
{
    return rain_slot_for(s_rain_latest - (time_t)(RAIN_FRAMES - 1 - p) * RAIN_STEP_S);
}

/* ARGB8888 as LVGL stores it (0xAARRGGBB), from 0xRRGGBB and an alpha. */
static uint32_t rain_argb(uint32_t rgb, uint32_t a)
{
    return (a << 24) | (rgb & 0xFFFFFF);
}

/* Draw the frame `lvl` (s_rain_crop's cells) into s_rain_px: each disc pixel
 * is placed on the crop (the disc uses the same flat local projection as the
 * coastline) and blended from the four cells around it, so the 1 km radar
 * pixels don't show as blocks. Where each pixel lands is interpolated from
 * s_rain_gx/gy. */
static void rain_render(const uint8_t *lvl)
{
    const int w = s_rain_crop.w, h = s_rain_crop.h;
    memset(s_rain_px, 0, (size_t)COAST_D * COAST_D * sizeof(uint32_t));
    for (int y = 0; y < COAST_D; y++) {
        const int dy = y - RADAR_R;
        const int half = (int)sqrtf((float)(RADAR_R * RADAR_R - dy * dy));
        const int gj = y / RAIN_GRID;
        const float ty = (float)(y - gj * RAIN_GRID) / RAIN_GRID;
        float ex[RAIN_GRID_N], ey[RAIN_GRID_N]; /* the grid, down at this row */
        for (int i = 0; i < RAIN_GRID_N; i++) {
            ex[i] = s_rain_gx[gj][i] + (s_rain_gx[gj + 1][i] - s_rain_gx[gj][i]) * ty;
            ey[i] = s_rain_gy[gj][i] + (s_rain_gy[gj + 1][i] - s_rain_gy[gj][i]) * ty;
        }
        uint32_t *row = s_rain_px + (size_t)y * COAST_D;
        for (int x = RADAR_R - half; x <= RADAR_R + half; x++) {
            const int gi = x / RAIN_GRID;
            const float tx = (float)(x - gi * RAIN_GRID) / RAIN_GRID;
            const float fx = ex[gi] + (ex[gi + 1] - ex[gi]) * tx;
            const float fy = ey[gi] + (ey[gi + 1] - ey[gi]) * tx;
            const int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            if (x0 < 0 || y0 < 0 || x0 + 1 >= w || y0 + 1 >= h) {
                continue;
            }
            const uint8_t *p = lvl + (size_t)y0 * w + x0;
            if ((p[0] | p[1] | p[w] | p[w + 1]) == 0) {
                continue; /* dry: the common case */
            }
            const float ux = fx - x0, uy = fy - y0;
            const float wt[4] = { (1 - ux) * (1 - uy), ux * (1 - uy), (1 - ux) * uy, ux * uy };
            const uint8_t lv[4] = { p[0], p[1], p[w], p[w + 1] };
            float acc[4] = { 0 };
            for (int k = 0; k < 4; k++) {
                for (int c = 0; c < 4; c++) {
                    acc[c] += wt[k] * s_rain_col[lv[k]][c];
                }
            }
            if (acc[3] < 0.02f) {
                continue;
            }
            row[x] = rain_argb(((uint32_t)(acc[0] / acc[3]) << 16) | ((uint32_t)(acc[1] / acc[3]) << 8) |
                                   (uint32_t)(acc[2] / acc[3]),
                               (uint32_t)(acc[3] * 255.0f));
        }
    }
}

/* Put frame position `p` (held in `slot`) on screen. Adapter lock held. */
static void rain_show(int p, int slot)
{
    rain_render(s_rain_frame[slot]);
    s_rain_pos = p;
    s_rain_time = s_rain_ftime[slot];
    s_rain_valid = true;
    lv_obj_invalidate(s_radar_canvas);
}

/* Step the rain animation on to the next frame the hour has, back to the
 * first after resting on the latest. Runs in the LVGL task. */
static void rain_anim_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_rain_mode || !s_rain_valid || s_rain_loading || s_radar_canvas == NULL ||
        lv_obj_has_flag(s_radar_root, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (s_rain_hold > 0) {
        s_rain_hold--;
        return;
    }
    for (int k = 1; k <= RAIN_FRAMES; k++) {
        int p = (s_rain_pos + k) % RAIN_FRAMES;
        int slot = rain_slot_at(p);
        if (slot < 0) {
            continue;
        }
        if (p != s_rain_pos) {
            rain_show(p, slot);
        }
        if (p == RAIN_FRAMES - 1) {
            s_rain_hold = RAIN_HOLD_TICKS;
        }
        return;
    }
}

/* The rain radar's table half: what the colours mean and where the data is from. */
static void rain_draw_legend(lv_layer_t *layer, int lh)
{
    /* Heaviest first, like MET's own legend. The levels are MET's
     * 5level_reflectivity classes: about 0.03, 0.1, 1, 2.5 and 5 mm/h. */
    static const char *const LABELS[RAIN_LEVELS] = {
        "Under 0,1 mm/t", "0,1 - 1 mm/t", "1 - 2,5 mm/t", "2,5 - 5 mm/t", "Over 5 mm/t",
    };
    const lv_color_t c_txt = lv_color_hex(s_rp->txt);
    const lv_color_t c_dim = lv_color_hex(s_rp->dim);
    const int row_h = 34;
    int y = 70;

    draw_line(layer, RADAR_LIST_X - 10, 48, RADAR_LIST_X - 10, 470, 1, lv_color_hex(s_rp->ring));
    draw_text(layer, "Nedb\xC3\xB8r", RADAR_LIST_X, y, 280, LV_TEXT_ALIGN_LEFT, c_dim);
    y += lh + 12;
    for (int lvl = RAIN_LEVELS; lvl >= RAIN_LEVEL_MIN; lvl--, y += row_h) {
        lv_area_t a = { RADAR_LIST_X, y + 2, RADAR_LIST_X + 40, y + lh - 2 };
        if (draw_area_visible(layer, &a)) {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.radius = 3;
            d.bg_opa = LV_OPA_COVER;
            d.bg_color = lv_color_hex(s_rp->rain[lvl - 1]);
            lv_draw_rect(layer, &d, &a);
        }
        draw_text(layer, LABELS[lvl - 1], RADAR_LIST_X + 54, y, 230, LV_TEXT_ALIGN_LEFT, c_txt);
    }

    y += 16;
    if (s_rain_valid && s_rain_time > PLAUSIBLE_EPOCH_S) {
        char when[40];
        struct tm lt;
        localtime_r(&s_rain_time, &lt);
        snprintf(when, sizeof(when), "Radarbilde kl. %02d:%02d", lt.tm_hour, lt.tm_min);
        draw_text(layer, when, RADAR_LIST_X, y, 280, LV_TEXT_ALIGN_LEFT, c_txt);
        y += lh + 4;

        /* Where the animation is in the hour: a tick per frame, the one on
         * show highlighted, faint where MET had no image. */
        for (int p = 0; p < RAIN_FRAMES; p++) {
            lv_area_t a = { RADAR_LIST_X + p * 21, y + 4, RADAR_LIST_X + p * 21 + 16, y + 12 };
            if (!draw_area_visible(layer, &a)) {
                continue;
            }
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.radius = 2;
            d.bg_color = (p == s_rain_pos) ? c_txt : lv_color_hex(s_rp->ring);
            d.bg_opa = (p == s_rain_pos || rain_slot_at(p) >= 0) ? LV_OPA_COVER : LV_OPA_20;
            lv_draw_rect(layer, &d, &a);
        }
        y += 24;
    }
    draw_text(layer, "Radar: MET Norge", RADAR_LIST_X, y, 280, LV_TEXT_ALIGN_LEFT, c_dim);
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
    const int lh = lv_font_get_line_height(g_font_body);
    const int range = s_radar_range_km;

    /* Grid: disc, range rings at 1/4 steps, crosshair, centre dot - with the
     * coastline under the rings. */
    radar_circle(layer, RADAR_R, 2, c_ring, true, c_disc);
    if (s_coast_valid &&
        draw_visible(layer, RADAR_CX - RADAR_R, RADAR_CY - RADAR_R, RADAR_CX + RADAR_R, RADAR_CY + RADAR_R)) {
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        lv_area_t a = { RADAR_CX - RADAR_R, RADAR_CY - RADAR_R,
                        RADAR_CX - RADAR_R + COAST_D - 1, RADAR_CY - RADAR_R + COAST_D - 1 };
        d.src = &s_water_img;
        d.recolor = lv_color_hex(s_rp->water); /* the colour of an A8 image */
        lv_draw_image(layer, &d, &a);
        if (s_rain_mode && s_rain_valid) {
            lv_draw_image_dsc_t r;
            lv_draw_image_dsc_init(&r);
            r.src = &s_rain_img;
            lv_draw_image(layer, &r, &a);
        }
        d.src = &s_coast_img;
        d.recolor = lv_color_hex(s_rp->coast);
        lv_draw_image(layer, &d, &a);
    }
    for (int i = 1; i < 4; i++) {
        radar_circle(layer, RADAR_R * i / 4, 1, c_ring, false, c_disc);
    }
    draw_line(layer, RADAR_CX - RADAR_R, RADAR_CY, RADAR_CX + RADAR_R, RADAR_CY, 1, c_ring);
    draw_line(layer, RADAR_CX, RADAR_CY - RADAR_R, RADAR_CX, RADAR_CY + RADAR_R, 1, c_ring);
    radar_circle(layer, 3, 0, c_txt, true, c_txt);

    /* Compass letters at the rim and the ring distances along the east spoke. */
    draw_text(layer, "N", RADAR_CX - 12, RADAR_CY - RADAR_R - lh + 1, 24, LV_TEXT_ALIGN_CENTER, c_txt);
    draw_text(layer, "S", RADAR_CX - 12, RADAR_CY + RADAR_R + 1, 24, LV_TEXT_ALIGN_CENTER, c_txt);
    draw_text(layer, "\xC3\x98", RADAR_CX + RADAR_R + 4, RADAR_CY - lh / 2, 24, LV_TEXT_ALIGN_LEFT, c_txt);
    draw_text(layer, "V", RADAR_CX - RADAR_R - 28, RADAR_CY - lh / 2, 24, LV_TEXT_ALIGN_RIGHT, c_txt);
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
            draw_text(layer, num, RADAR_CX + RADAR_R * i / 4 - 70, RADAR_CY - lh - 1, 70,
                       LV_TEXT_ALIGN_RIGHT, c_dim);
        } else {
            int y = RADAR_CY - 2 * lh - 1;
            float dy = (float)(RADAR_CY - (y + lh / 2)); /* label centre above the spoke */
            int ring_x = RADAR_CX + (int)lroundf(sqrtf((float)(RADAR_R * RADAR_R) - dy * dy));
            draw_text(layer, num, ring_x - 4 - 60, y, 60, LV_TEXT_ALIGN_RIGHT, c_dim);
            draw_text(layer, "km", ring_x + 4, y, 34, LV_TEXT_ALIGN_LEFT, c_dim);
        }
    }

    if (s_coast_valid) {
        draw_text(layer, "\xC2\xA9 OpenStreetMap", 8, 480 - lh - 4, 200, LV_TEXT_ALIGN_LEFT, c_dim);
    }
    if (s_ship_mode) {
        ships_draw(layer, range, lh);
        return;
    }
    if (s_rain_mode) {
        rain_draw_legend(layer, lh);
        return;
    }

    radar_draw_airports(layer, range);

    /* Table header + divider. */
    draw_line(layer, RADAR_LIST_X - 10, 48, RADAR_LIST_X - 10, 470, 1, c_ring);
    radar_cell(layer, &s_ac_col[AC_CALL], "Fly", 50, LV_TEXT_ALIGN_LEFT, c_dim);
    radar_cell(layer, &s_ac_col[AC_TYPE], s_route_short ? "Til" : "Rute", 50, LV_TEXT_ALIGN_LEFT, c_dim);
    radar_cell(layer, &s_ac_col[AC_ALT], "H\xC3\xB8yde", 50, LV_TEXT_ALIGN_RIGHT, c_dim);
    radar_cell(layer, &s_ac_col[AC_GS], "kt", 50, LV_TEXT_ALIGN_RIGHT, c_dim);
    radar_cell(layer, &s_ac_col[AC_DIST], "km", 50, LV_TEXT_ALIGN_RIGHT, c_dim);

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
            draw_line(layer, tri[0][0], tri[0][1],
                       px + (int)lroundf(fx * (9 + vec_px)), py + (int)lroundf(fy * (9 + vec_px)),
                       1, c_vec);
        }
        draw_triangle(layer, tri, c_plane);

        /* Callsign tag for the nearest few, on the side facing the centre;
         * skipped if it would land on a tag already placed. */
        if (i < RADAR_TAGS) {
            lv_point_t sz;
            lv_text_get_size(&sz, a->callsign, g_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
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
                draw_text(layer, a->callsign, tag.x1, tag.y1, sz.x + 2, LV_TEXT_ALIGN_LEFT, c_txt);
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
        draw_text(layer, none, RADAR_LIST_X, RADAR_LIST_Y, RADAR_LIST_R - RADAR_LIST_X, LV_TEXT_ALIGN_LEFT, c_txt);
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
        radar_cell(layer, &s_ac_col[AC_CALL], a->callsign, y, LV_TEXT_ALIGN_LEFT, c_txt);
        /* The route when it's known, else the aircraft type (not in the
         * narrow destination-only column). */
        if (a->route[0] != '\0') {
            const char *dest = strrchr(a->route, '-');
            radar_cell(layer, &s_ac_col[AC_TYPE], (s_route_short && dest) ? dest + 1 : a->route, y,
                       LV_TEXT_ALIGN_LEFT, c_txt);
        } else if (!s_route_short) {
            radar_cell(layer, &s_ac_col[AC_TYPE], a->type, y, LV_TEXT_ALIGN_LEFT, c_dim);
        }
        radar_cell(layer, &s_ac_col[AC_ALT], alt, y, LV_TEXT_ALIGN_RIGHT, c_txt);
        radar_cell(layer, &s_ac_col[AC_GS], gs, y, LV_TEXT_ALIGN_RIGHT, c_txt);
        radar_cell(layer, &s_ac_col[AC_DIST], dist, y, LV_TEXT_ALIGN_RIGHT, c_txt);
    }
    if (res->total > res->count) {
        char more[40];
        snprintf(more, sizeof(more), "Viser %d av %d fly", res->count, res->total);
        draw_text(layer, more, RADAR_LIST_X, RADAR_LIST_Y + RADAR_LIST_ROWS * RADAR_LIST_ROW_H + 4,
                   RADAR_LIST_R - RADAR_LIST_X, LV_TEXT_ALIGN_LEFT, c_dim);
    }
}

/* The ship count line, e.g. "2 skip lengre enn 100 meter innen 20 km",
 * mentioning the length filter only when one is configured, and the inner
 * zone's own filter when it differs. */
static void ship_info_set(int total)
{
    const int loc = s_radar_loc;
    const unsigned min_len = (loc >= 0) ? g_cfg->ship_min_len_m[loc] : 0;
    const unsigned near_km = (loc >= 0) ? g_cfg->ship_near_km[loc] : 0;
    const unsigned near_len = (loc >= 0) ? g_cfg->ship_near_min_len_m[loc] : 0;
    if (near_km > 0 && near_km < (unsigned)s_radar_range_km && near_len != min_len) {
        char near[40];
        if (near_len > 0) {
            snprintf(near, sizeof(near), "over %u m innen %u km", near_len, near_km);
        } else {
            snprintf(near, sizeof(near), "alle innen %u km", near_km);
        }
        if (min_len > 0) {
            lv_label_set_text_fmt(s_radar_info, "%d skip: %s, over %u m innen %u km",
                                  total, near, min_len, (unsigned)s_radar_range_km);
        } else {
            lv_label_set_text_fmt(s_radar_info, "%d skip: %s, alle innen %u km",
                                  total, near, (unsigned)s_radar_range_km);
        }
        return;
    }
    /* One filter over the whole range: the inner zone's if it covers it. */
    const unsigned len = (near_km >= (unsigned)s_radar_range_km) ? near_len : min_len;
    if (len > 0) {
        lv_label_set_text_fmt(s_radar_info, "%d skip lengre enn %u meter innen %u km",
                              total, len, (unsigned)s_radar_range_km);
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

/* The info line in orange while the last poll failed and older data is
 * shown (adapter lock held). */
static void info_stale(bool stale)
{
    if (stale) {
        lv_obj_set_style_text_color(s_radar_info, lv_color_hex(0xE07000), 0);
    } else {
        lv_obj_remove_local_style_prop(s_radar_info, LV_STYLE_TEXT_COLOR, 0);
    }
}

/* Show `res`, fetched at `at`. Called with the adapter lock held. */
static void ships_apply(const ais_result_t *res, const fetch_stamp_t *at)
{
    memcpy(s_ship_data, res, sizeof(*res));
    s_radar_valid = true;
    s_radar_tick = at->tick;
    ship_info_set(res->total);
    lv_obj_invalidate(s_radar_canvas);
}

/* Show `res`, fetched at `at`. Called with the adapter lock held. */
static void radar_apply(const adsb_result_t *res, const fetch_stamp_t *at)
{
    memcpy(s_radar_data, res, sizeof(*res));
    s_radar_valid = true;
    s_radar_tick = at->tick;

    char info[64];
    if (at->when > 0) {
        struct tm lt;
        localtime_r(&at->when, &lt);
        snprintf(info, sizeof(info), "%d fly innen %u km  kl. %02d:%02d",
                 res->total, (unsigned)s_radar_range_km, lt.tm_hour, lt.tm_min);
    } else {
        snprintf(info, sizeof(info), "%d fly innen %u km", res->total, (unsigned)s_radar_range_km);
    }
    lv_label_set_text(s_radar_info, info);
    lv_obj_align(s_radar_info, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_obj_invalidate(s_radar_canvas);
}

lv_obj_t *radar_build(lv_obj_t *screen)
{
    lv_obj_t *root = s_radar_root = screen_root_create(screen);
    s_adsb_scratch = heap_caps_malloc(sizeof(*s_adsb_scratch), MALLOC_CAP_SPIRAM);
    s_ais_scratch = heap_caps_malloc(sizeof(*s_ais_scratch), MALLOC_CAP_SPIRAM);
    assert(s_adsb_scratch != NULL && s_ais_scratch != NULL);
    for (int i = 0; i < g_cfg->location_count; i++) {
        if (g_cfg->show[i] & APP_SHOW_RADAR) {
            s_adsb_cache[i] = heap_caps_malloc(sizeof(adsb_result_t), MALLOC_CAP_SPIRAM);
        }
        if (g_cfg->show[i] & APP_SHOW_SHIPS) {
            s_ais_cache[i] = heap_caps_malloc(sizeof(ais_result_t), MALLOC_CAP_SPIRAM);
        }
    }
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
    bool any_rain = false;
    for (int i = 0; i < g_cfg->location_count; i++) {
        any_rain |= (g_cfg->show[i] & APP_SHOW_RAIN) != 0;
    }
    for (int i = 0; i < RAIN_FRAMES; i++) {
        s_rain_ftime[i] = RAIN_EMPTY;
    }
    s_rain_px = any_rain ? heap_caps_calloc((size_t)COAST_D * COAST_D, sizeof(uint32_t), MALLOC_CAP_SPIRAM)
                         : NULL;
    s_rain_img = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
                    .w = COAST_D, .h = COAST_D, .stride = COAST_D * 4 },
        .data = (const uint8_t *)s_rain_px,
        .data_size = COAST_D * COAST_D * 4,
    };
    s_radar_apt = heap_caps_calloc(RADAR_APT_MAX, sizeof(*s_radar_apt), MALLOC_CAP_SPIRAM);
    s_radar_rwy = heap_caps_calloc(RADAR_APT_MAX * RADAR_APT_RWY_MAX, sizeof(*s_radar_rwy), MALLOC_CAP_SPIRAM);
    assert(s_radar_data != NULL && s_ship_data != NULL && s_coast_px != NULL && s_water_px != NULL && s_water_seeds != NULL &&
           (s_rain_px != NULL || !any_rain) &&
           s_radar_apt != NULL && s_radar_rwy != NULL);

    /* A screen of its own in the theme's radar palette. Text colour is
     * inherited by the labels below. */
    s_rp = (g_cfg->theme == APP_THEME_DARK) ? &RADAR_DARK : &RADAR_LIGHT;
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
    lv_obj_set_style_text_font(s_radar_title, g_font_large, 0);
    lv_obj_set_pos(s_radar_title, 12, 4);
    lv_label_set_text(s_radar_title, "");

    s_radar_info = lv_label_create(root);
    lv_obj_align(s_radar_info, LV_ALIGN_TOP_RIGHT, -12, 4);
    lv_label_set_text(s_radar_info, "");

    lv_timer_create(radar_redraw_timer_cb, RADAR_REDRAW_MS, NULL);
    radar_layout_tables();
    ESP_LOGI(TAG, "Radar table: callsign %d px%s%s, name %d px", s_ac_col[AC_CALL].w,
             s_ac_col[AC_TYPE].w ? "" : " (no route/type column)",
             s_ac_col[AC_GS].w ? "" : (s_ac_col[AC_DIST].w ? " (no speed column)" : " (no speed or distance column)"),
             s_sh_col[SH_NAME].w);

    for (int l = 1; l <= RAIN_LEVELS; l++) {
        const uint32_t c = s_rp->rain[l - 1];
        const float al = (l == 1) ? 0.6f : 0.85f;
        s_rain_col[l][0] = al * ((c >> 16) & 0xFF);
        s_rain_col[l][1] = al * ((c >> 8) & 0xFF);
        s_rain_col[l][2] = al * (c & 0xFF);
        s_rain_col[l][3] = al;
    }
    if (any_rain) {
        lv_timer_create(rain_anim_timer_cb, RAIN_ANIM_MS, NULL);
    }
    return root;
}

/* Point the radar at location `loc` (adapter lock held): its title, and the
 * airports within range. */
static void radar_set_location(int loc)
{
    s_ship_mode = false;
    s_rain_mode = false;
    lv_label_set_text_fmt(s_radar_title, "Fly n\xC3\xA6r %s", g_cfg->locations[loc].name);
    s_radar_range_km = g_cfg->radar_km[loc];
    coast_mark(loc, &g_cfg->locations[loc], s_radar_range_km);
    radar_load_airports(atof(g_cfg->locations[loc].lat), atof(g_cfg->locations[loc].lon));
}

/* Point the radar screen at location `loc`'s ship traffic (adapter lock
 * held), around its own centre for ships if it has one. */
static void ships_set_location(int loc)
{
    const app_location_t *at = app_config_ship_centre(g_cfg, loc);
    s_ship_mode = true;
    s_rain_mode = false;
    lv_label_set_text_fmt(s_radar_title, "Skip n\xC3\xA6r %s", at->name[0] ? at->name : g_cfg->locations[loc].name);
    s_radar_range_km = g_cfg->ship_km[loc];
    coast_mark(loc, at, s_radar_range_km);
}

/* Point the radar screen at location `loc`'s rain radar (adapter lock held).
 * The hour of frames still held for it is kept if recent enough (the poll
 * then only fetches what's new); otherwise blank until the first image for
 * it lands. */
static void rain_set_location(int loc)
{
    s_ship_mode = false;
    s_rain_mode = true;
    const time_t now = time(NULL);
    const bool keep = s_rain_valid && loc == s_rain_prep_loc && g_cfg->rain_km[loc] == s_rain_prep_range &&
                      now > PLAUSIBLE_EPOCH_S && now - s_rain_latest < RAIN_KEEP_S;
    if (!keep) {
        s_rain_valid = false;
        s_rain_pos = -1;
        s_rain_hold = 0;
        s_rain_latest = 0;
        for (int i = 0; i < RAIN_FRAMES; i++) {
            s_rain_ftime[i] = RAIN_EMPTY;
        }
    }
    lv_label_set_text_fmt(s_radar_title, "Nedb\xC3\xB8r n\xC3\xA6r %s", g_cfg->locations[loc].name);
    s_radar_range_km = g_cfg->rain_km[loc];
    coast_mark(loc, &g_cfg->locations[loc], s_radar_range_km);
}

/* One ADS-B poll for the radar of location `loc`, shown only if the screen is
 * still `for_view` when the reply lands. The previous plot is kept on a failed
 * poll (it just keeps dead-reckoning) unless there is none yet. */
static void aircraft_poll(int loc, adsb_result_t *scratch, int for_view)
{
    double lat = atof(g_cfg->locations[loc].lat);
    double lon = atof(g_cfg->locations[loc].lon);
    esp_err_t err = adsb_client_fetch(lat, lon, (float)g_cfg->radar_km[loc], scratch);
    const bool ok = (err == ESP_OK);
    if (ok) {
        diag_ok(DIAG_AIRCRAFT);
        /* Routes for the table's rows, a few new ones per poll. */
        if (s_ac_col[AC_TYPE].w > 0) {
            esp_err_t rerr = adsb_routes_fill(scratch, RADAR_LIST_ROWS, 4);
            if (rerr == ESP_OK) {
                diag_ok(DIAG_ROUTES);
            } else {
                diag_fail(DIAG_ROUTES, rerr);
            }
        }
    } else {
        diag_fail(DIAG_AIRCRAFT, err);
    }

    if (!lock_for_view(for_view)) {
        return;
    }
    if (ok) {
        lv_label_set_text(g_status_label, "");
        stamp_now(&s_adsb_at[loc]);
        if (s_adsb_cache[loc] != NULL) {
            memcpy(s_adsb_cache[loc], scratch, sizeof(*scratch));
        }
        radar_apply(scratch, &s_adsb_at[loc]);
        info_stale(false);
    } else if (!s_radar_valid) {
        lv_label_set_text(g_status_label, "Kunne ikke hente fly. Pr\xC3\xB8ver igjen...");
    } else {
        info_stale(true);
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
enum { WATER_UNKNOWN, WATER_SEA, WATER_LAND, WATER_COAST, WATER_COAST_SEA_TMP, WATER_SEEN = 0x20, WATER_DONE = 0x80,
       WATER_DONE_SEA = 0xC0 };

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
    s_water_seeds[s_water_n_seeds++] = (water_seed_t){ (int16_t)lroundf(mx * SEED_POS), (int16_t)lroundf(my * SEED_POS),
                                                       (int8_t)lroundf(dy / len * SEED_DIR),
                                                       (int8_t)lroundf(-dx / len * SEED_DIR) };
}

/* From a segment's middle, step along `dir` off the line and mark the first
 * pixel clear of it as `v`, unless another line comes first. */
static void water_mark_side(const water_seed_t *sd, float dir, uint8_t v)
{
    const float sx = sd->x / SEED_POS, sy = sd->y / SEED_POS, nx = sd->nx / SEED_DIR, ny = sd->ny / SEED_DIR;
    for (float t = 0.5f; t <= 4.0f; t += 0.5f) {
        int x = (int)lroundf(sx + dir * nx * t), y = (int)lroundf(sy + dir * ny * t);
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

/* The flood fill's queue: a ring of pixel indices, which only ever holds
 * the edge of the area being flooded (a few thousand pixels at most on a
 * 353 px disc), not the whole area. Static in PSRAM: a run-time block of
 * the whole image (500 KB) often wasn't there once the rain radar's frames
 * and the image cache held theirs, and the water was then left out. */
#define WATER_RING 16384
static EXT_RAM_BSS_ATTR uint32_t s_water_ring[WATER_RING];

/* Visit the area holding pixel `start` (4-connected, not crossing coast
 * pixels): every pixel without a bit of `stop` gets `set` ORed in, or is
 * replaced by `set` if `replace`. Counts the sea and land seed pixels met. */
static void water_flood(int start, uint8_t stop, uint8_t set, bool replace, int *sea, int *land)
{
    static const int8_t nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    static bool warned;
    uint32_t head = 0, tail = 0;
    const uint8_t v0 = s_water_px[start] & 0x0F;
    *sea += (v0 == WATER_SEA);
    *land += (v0 == WATER_LAND);
    s_water_px[start] = replace ? set : (s_water_px[start] | set);
    s_water_ring[tail++ % WATER_RING] = (uint32_t)start;
    while (head != tail) {
        const int i = (int)s_water_ring[head++ % WATER_RING];
        const int x = i % COAST_D, y = i / COAST_D;
        for (int k = 0; k < 4; k++) {
            const int nx = x + nb[k][0], ny = y + nb[k][1];
            if (!coast_in_disc(nx, ny)) {
                continue;
            }
            uint8_t *w = &s_water_px[ny * COAST_D + nx];
            if (*w == WATER_COAST || (*w & stop)) {
                continue;
            }
            if (tail - head >= WATER_RING) {
                if (!warned) {
                    warned = true;
                    ESP_LOGW(TAG, "Water fill: queue full, an area is left partly unfilled");
                }
                continue;
            }
            const uint8_t v = *w & 0x0F;
            *sea += (v == WATER_SEA);
            *land += (v == WATER_LAND);
            *w = replace ? set : (*w | set);
            s_water_ring[tail++ % WATER_RING] = (uint32_t)(ny * COAST_D + nx);
        }
    }
}

/* Split the disc into the areas the coastline in s_coast_px separates, and
 * make each area sea or land by a vote of the seed pixels inside it (a few
 * seeds land on the wrong side where the coast bends tightly). An area with no
 * seeds (no coast in view) stays unfilled; coastline pixels take the side most
 * of their neighbours are on. Each area is flooded twice: once to count its
 * votes (marking it WATER_SEEN), once to fill it. */
static void coast_fill_water(void)
{
    const int n = COAST_D * COAST_D;
    for (int i = 0; i < n; i++) {
        s_water_px[i] = s_coast_px[i] > 0 ? WATER_COAST : WATER_UNKNOWN;
    }
    for (int i = 0; i < s_water_n_seeds; i++) {
        water_mark_side(&s_water_seeds[i], 1.0f, WATER_SEA);
        water_mark_side(&s_water_seeds[i], -1.0f, WATER_LAND);
    }
    static const int8_t nb[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (int start = 0; start < n; start++) {
        const uint8_t v0 = s_water_px[start];
        if (v0 == WATER_COAST || (v0 & (WATER_DONE | WATER_SEEN)) || !coast_in_disc(start % COAST_D, start / COAST_D)) {
            continue;
        }
        int sea = 0, land = 0, unused = 0;
        water_flood(start, WATER_DONE | WATER_SEEN, WATER_SEEN, false, &sea, &land);
        water_flood(start, WATER_DONE, (sea > land) ? WATER_DONE_SEA : WATER_DONE, true, &unused, &unused);
    }
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

/* Draw the coastline around `at` (a location in g_cfg, or its centre for
 * ships) at `range` km into s_coast_px, unless it's already there. Runs in the weather task; flash reads, so not under the
 * adapter lock except to flip s_coast_valid. */
static void coast_render(const app_location_t *at, int range)
{
    static const esp_partition_t *part;
    static coast_hdr_t hdr;
    if (s_coast_px == NULL || (s_coast_at == at && s_coast_km == range && s_coast_valid && !s_coast_retry)) {
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

    const double lat0 = atof(at->lat);
    const double lon0 = atof(at->lon);
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
    bool complete = true; /* false: out of memory or a flash read failed */

    for (int r = r0; r <= r1 && c1 >= c0 && c1 - c0 + 2 <= 64; r++) {
        int n_idx = c1 - c0 + 2;
        if (esp_partition_read(part, index_off + 4 * ((size_t)r * hdr.cols + c0), idx, 4 * n_idx) != ESP_OK) {
            complete = false;
            break;
        }
        size_t len = idx[n_idx - 1] - idx[0];
        if (len == 0) {
            continue;
        }
        if (len > buf_cap) {
            uint8_t *nb = heap_caps_realloc(buf, len, MALLOC_CAP_SPIRAM);
            if (nb == NULL) {
                complete = false;
                break;
            }
            buf = nb;
            buf_cap = len;
        }
        if (esp_partition_read(part, data_off + idx[0], buf, len) != ESP_OK) {
            complete = false;
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
                        complete = false;
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
    ESP_LOGI(TAG, "Coastline around %s (%s, %s): %d line(s) in tiles %d-%d x %d-%d%s",
             at->name, at->lat, at->lon, lines, r0, r1, c0, c1, complete ? "" : " - incomplete, redone next poll");

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        s_coast_at = at;
        s_coast_km = range;
        /* Unless the screen moved on while drawing, or it's incomplete: then
         * it's drawn again on the next poll (it's still shown meanwhile). */
        s_coast_valid = (s_radar_at == at && s_radar_range_km == range);
        s_coast_retry = !complete;
        lv_obj_invalidate(s_radar_canvas);
        esp_lv_adapter_unlock();
    }
}

static void ships_poll(int loc, ais_result_t *scratch, int for_view)
{
    const app_location_t *at = app_config_ship_centre(g_cfg, loc);
    double lat = atof(at->lat);
    double lon = atof(at->lon);
    esp_err_t err = ais_client_fetch(g_cfg->ais_client_id, g_cfg->ais_client_secret,
                                     lat, lon, (float)g_cfg->ship_km[loc], g_cfg->ship_min_len_m[loc],
                                     (float)g_cfg->ship_near_km[loc], g_cfg->ship_near_min_len_m[loc],
                                     scratch);
    if (err == ESP_OK) {
        diag_ok(DIAG_SHIPS);
    } else {
        diag_fail(DIAG_SHIPS, err);
    }

    if (!lock_for_view(for_view)) {
        return;
    }
    if (err == ESP_OK) {
        lv_label_set_text(g_status_label, "");
        stamp_now(&s_ais_at[loc]);
        if (s_ais_cache[loc] != NULL) {
            memcpy(s_ais_cache[loc], scratch, sizeof(*scratch));
        }
        ships_apply(scratch, &s_ais_at[loc]);
        info_stale(false);
    } else if (err == ESP_ERR_INVALID_ARG) {
        lv_label_set_text(g_status_label, "Mangler BarentsWatch-n\xC3\xB8kkel");
    } else if (err == ESP_ERR_INVALID_STATE) {
        lv_label_set_text(g_status_label, "Innlogging feilet");
    } else if (!s_radar_valid) {
        lv_label_set_text(g_status_label, "Kunne ikke hente skip. Pr\xC3\xB8ver igjen...");
    } else {
        info_stale(true);
    }
    esp_lv_adapter_unlock();
}

/* --------------------------------------------------------------------------
 * Rain radar: MET's latest radar image of the area around a location
 * (rain_client), redrawn over the radar disc in the theme's rain colours.
 * ------------------------------------------------------------------------ */

/* The line above the legend: what the disc shows. Adapter lock held. */
static void rain_info_set(int range)
{
    if (s_rain_loading) {
        lv_label_set_text(s_radar_info, "Henter siste time...");
    } else {
        lv_label_set_text_fmt(s_radar_info, "Nedb\xC3\xB8r siste time innen %d km", range);
    }
    lv_obj_align(s_radar_info, LV_ALIGN_TOP_RIGHT, -12, 4);
}

/* Keep the frame just fetched into s_rain_spare, taken at `t`, with
 * `latest` the newest image there is. Adapter lock held. */
static void rain_store(time_t t, time_t latest, int range)
{
    s_rain_latest = latest;
    int slot = rain_slot_for(t);
    for (int i = 0; slot < 0 && i < RAIN_FRAMES; i++) {
        /* Free, or fallen out of the hour: the hour has RAIN_FRAMES
         * positions and `t` is one of them not yet held, so there is one. */
        const time_t ft = s_rain_ftime[i];
        if (ft == RAIN_EMPTY || ft > latest || ft < latest - (time_t)(RAIN_FRAMES - 1) * RAIN_STEP_S) {
            slot = i;
        }
    }
    if (slot < 0) {
        return;
    }
    uint8_t *old = s_rain_frame[slot];
    s_rain_frame[slot] = s_rain_spare;
    s_rain_spare = old;
    s_rain_ftime[slot] = t;

    const int p = RAIN_FRAMES - 1 - (int)((latest - t) / RAIN_STEP_S);
    if (!s_rain_valid) {
        rain_show(p, slot); /* the first to land; the animation takes it from here */
        s_rain_hold = RAIN_HOLD_TICKS;
    } else if (p == s_rain_pos) {
        rain_show(p, slot);
    }
    lv_label_set_text(g_status_label, "");
    rain_info_set(range);
    lv_obj_invalidate(s_radar_canvas);
}

/* Work out the part of `area` under location `loc`'s disc at `range` km and
 * where the disc lands on it (s_rain_crop, s_rain_gx/gy), and have frame
 * buffers for it. Frames held for another crop are dropped. Only redone when
 * the location, range or area changes. False if out of memory. */
static bool rain_prepare(int loc, int range, const rain_area_t *area)
{
    if (loc == s_rain_prep_loc && range == s_rain_prep_range && area == s_rain_area) {
        return true;
    }

    const double lat0 = atof(g_cfg->locations[loc].lat);
    const double lon0 = atof(g_cfg->locations[loc].lon);
    const double km_per_px = (double)range / RADAR_R;
    const double km_lon = 111.320 * cos(lat0 * M_PI / 180.0);
    static EXT_RAM_BSS_ATTR float gx[RAIN_GRID_N][RAIN_GRID_N];
    static EXT_RAM_BSS_ATTR float gy[RAIN_GRID_N][RAIN_GRID_N];
    float x_lo = 1e9f, x_hi = -1e9f, y_lo = 1e9f, y_hi = -1e9f;
    for (int j = 0; j < RAIN_GRID_N; j++) {
        for (int i = 0; i < RAIN_GRID_N; i++) {
            double dx = (i * RAIN_GRID - RADAR_R) * km_per_px;
            double dy = (RADAR_R - j * RAIN_GRID) * km_per_px;
            rain_client_project(area, lat0 + dy / 110.574, lon0 + dx / km_lon, &gx[j][i], &gy[j][i]);
            x_lo = fminf(x_lo, gx[j][i]);
            x_hi = fmaxf(x_hi, gx[j][i]);
            y_lo = fminf(y_lo, gy[j][i]);
            y_hi = fmaxf(y_hi, gy[j][i]);
        }
    }

    /* A pixel of margin all round for the blending; coarser cells if the
     * hour of frames wouldn't fit in RAIN_BUDGET, or in what PSRAM has
     * beyond RAIN_RESERVE (counting the frames held now, which go first). */
    const size_t held = s_rain_cells * (RAIN_FRAMES + 1);
    const size_t avail = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) + held;
    const size_t budget = avail > RAIN_RESERVE + RAIN_BUDGET ? RAIN_BUDGET
                        : avail > RAIN_RESERVE + 64 * 1024 ? avail - RAIN_RESERVE
                                                           : 64 * 1024;
    rain_crop_t c = { .x0 = (int)floorf(x_lo) - 1, .y0 = (int)floorf(y_lo) - 1, .step = 1 };
    const int px_w = (int)ceilf(x_hi) + 2 - c.x0, px_h = (int)ceilf(y_hi) + 2 - c.y0;
    for (;; c.step++) {
        c.w = (uint16_t)((px_w + c.step - 1) / c.step);
        c.h = (uint16_t)((px_h + c.step - 1) / c.step);
        if ((size_t)c.w * c.h * (RAIN_FRAMES + 1) <= budget) {
            break;
        }
    }

    /* Drop the frames held (nothing reads a buffer once its frame is gone)
     * and, if they're too small, the buffers too - freed and reallocated
     * outside the lock, so the screen and taps aren't held up. */
    const size_t cells = (size_t)c.w * c.h;
    const bool grow = cells > s_rain_cells;
    uint8_t *old[RAIN_FRAMES + 1] = { 0 };
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return false;
    }
    s_rain_area = NULL; /* not ready until the end */
    s_rain_crop = c;
    for (int j = 0; j < RAIN_GRID_N; j++) {
        for (int i = 0; i < RAIN_GRID_N; i++) {
            /* Cell cx's middle is at image pixel x0 + cx * step + (step - 1) / 2. */
            s_rain_gx[j][i] = (gx[j][i] - c.x0 - (c.step - 1) * 0.5f) / c.step;
            s_rain_gy[j][i] = (gy[j][i] - c.y0 - (c.step - 1) * 0.5f) / c.step;
        }
    }
    for (int i = 0; i < RAIN_FRAMES; i++) {
        s_rain_ftime[i] = RAIN_EMPTY;
    }
    s_rain_valid = false;
    s_rain_pos = -1;
    if (grow) {
        for (int i = 0; i < RAIN_FRAMES; i++) {
            old[i] = s_rain_frame[i];
            s_rain_frame[i] = NULL;
        }
        old[RAIN_FRAMES] = s_rain_spare;
        s_rain_spare = NULL;
        s_rain_cells = 0;
    }
    esp_lv_adapter_unlock();

    bool ok = true;
    if (grow) {
        uint8_t *fresh[RAIN_FRAMES + 1];
        for (int i = 0; i <= RAIN_FRAMES; i++) {
            heap_caps_free(old[i]);
        }
        for (int i = 0; i <= RAIN_FRAMES; i++) {
            fresh[i] = heap_caps_malloc(cells, MALLOC_CAP_SPIRAM);
            ok &= (fresh[i] != NULL);
        }
        if (!ok) {
            for (int i = 0; i <= RAIN_FRAMES; i++) {
                heap_caps_free(fresh[i]);
            }
        } else if (esp_lv_adapter_lock(-1) == ESP_OK) {
            memcpy(s_rain_frame, fresh, sizeof(s_rain_frame));
            s_rain_spare = fresh[RAIN_FRAMES];
            s_rain_cells = cells;
            esp_lv_adapter_unlock();
        } else {
            ok = false;
        }
    }
    ESP_LOGI(TAG, "Rain: %s, %ux%u cells of %u px from (%d, %d)%s; PSRAM free %u", area->name, c.w, c.h,
             c.step, c.x0, c.y0, ok ? "" : ", out of memory",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (ok) {
        s_rain_area = area;
        s_rain_prep_loc = loc;
        s_rain_prep_range = range;
    }
    return ok; /* else tried again on the next poll */
}

/* Fetch the rain radar for location `loc`: the latest image, then whatever
 * of the hour before it isn't held yet, newest first. Each frame goes on
 * screen as it lands, as long as the screen is still `for_view`; the
 * previous ones stay up on a failed fetch. True once there is something on
 * screen. */
static bool rain_poll(int loc, int for_view)
{
    const double lat = atof(g_cfg->locations[loc].lat);
    const double lon = atof(g_cfg->locations[loc].lon);
    const int range = g_cfg->rain_km[loc];
    const rain_area_t *area = rain_client_pick_area(lat, lon, (float)range);
    const bool ready = (area != NULL) && rain_prepare(loc, range, area);

    time_t latest = 0;
    for (int k = 0; ready && k < RAIN_FRAMES && g_view_index == for_view; k++) {
        time_t want = 0;
        if (k > 0) {
            if (latest <= PLAUSIBLE_EPOCH_S) {
                break; /* no image time: nothing to count back from */
            }
            want = latest - (time_t)k * RAIN_STEP_S;
            if (rain_slot_for(want) >= 0) {
                continue;
            }
            /* Frames of the hour still to come: hold the latest still
             * until they're all in, rather than animate a patchy hour. */
            if (!s_rain_loading && lock_for_view(for_view)) {
                s_rain_loading = true;
                const int slot = rain_slot_for(latest);
                if (slot >= 0) {
                    rain_show(RAIN_FRAMES - 1, slot);
                }
                rain_info_set(range);
                esp_lv_adapter_unlock();
            }
        }
        time_t taken;
        esp_err_t err = rain_client_fetch(area, want, &s_rain_crop, s_rain_spare, &taken);
        if (k == 0) {
            if (err == ESP_OK) {
                diag_ok(DIAG_RAIN);
            } else {
                diag_fail(DIAG_RAIN, err);
            }
        }
        if (err != ESP_OK) {
            if (k == 0) {
                break;
            }
            continue; /* a gap in the hour; the animation skips it */
        }
        if (k == 0) {
            latest = taken;
            if (latest == s_rain_latest && rain_slot_for(latest) >= 0) {
                continue; /* no new image since the last poll */
            }
        }
        if (lock_for_view(for_view)) {
            if (s_radar_loc == loc && s_radar_range_km == range) {
                rain_store(taken, latest, range);
            }
            esp_lv_adapter_unlock();
        }
    }

    if (s_rain_loading && esp_lv_adapter_lock(-1) == ESP_OK) {
        /* Done (or the screen moved on): play the hour from its start. */
        s_rain_loading = false;
        s_rain_hold = 0;
        rain_info_set(range);
        esp_lv_adapter_unlock();
    }

    bool shown = false;
    if (lock_for_view(for_view)) {
        if (area == NULL) {
            lv_label_set_text(g_status_label, "Ingen nedb\xC3\xB8rsradar her");
        } else if (!s_rain_valid) {
            lv_label_set_text(g_status_label, "Kunne ikke hente nedb\xC3\xB8r. Pr\xC3\xB8ver igjen...");
        }
        shown = s_rain_valid;
        esp_lv_adapter_unlock();
    }
    return shown;
}

uint32_t radar_status_colour(void)
{
    return s_rp->txt;
}

void radar_enter(int kind, int loc)
{
    info_stale(false);
    switch (kind) {
    case STOP_RADAR:
        radar_set_location(loc);
        lv_label_set_text(s_radar_info, "");
        /* Never another location's aircraft: blank unless this one's are
         * recent. */
        s_radar_valid = s_adsb_cache[loc] != NULL && stamp_fresh(&s_adsb_at[loc], RADAR_CACHE_MAX_MS);
        if (s_radar_valid) {
            radar_apply(s_adsb_cache[loc], &s_adsb_at[loc]);
        }
        lv_obj_invalidate(s_radar_canvas);
        lv_label_set_text(g_status_label, s_radar_valid ? "" : "Henter fly...");
        break;
    case STOP_SHIPS:
        ships_set_location(loc);
        lv_label_set_text(s_radar_info, "");
        s_radar_valid = s_ais_cache[loc] != NULL && stamp_fresh(&s_ais_at[loc], SHIP_CACHE_MAX_MS);
        if (s_radar_valid) {
            ships_apply(s_ais_cache[loc], &s_ais_at[loc]);
        }
        lv_obj_invalidate(s_radar_canvas);
        lv_label_set_text(g_status_label, s_radar_valid ? "" : "Henter skip...");
        break;
    case STOP_RAIN:
        s_radar_valid = false; /* nothing to dead-reckon: no periodic redraw */
        rain_set_location(loc);
        lv_label_set_text(s_radar_info, "");
        if (s_rain_valid) {
            rain_info_set(g_cfg->rain_km[loc]); /* the hour kept from last time */
        }
        lv_obj_invalidate(s_radar_canvas);
        lv_label_set_text(g_status_label, s_rain_valid ? "" : "Henter nedb\xC3\xB8r...");
        break;
    }
}

uint32_t radar_poll(int kind, int loc, int for_view)
{
    switch (kind) {
    case STOP_RADAR:
        coast_render(&g_cfg->locations[loc], g_cfg->radar_km[loc]);
        aircraft_poll(loc, s_adsb_scratch, for_view);
        return ADSB_POLL_MS;
    case STOP_SHIPS:
        coast_render(app_config_ship_centre(g_cfg, loc), g_cfg->ship_km[loc]);
        ships_poll(loc, s_ais_scratch, for_view);
        return SHIP_POLL_MS;
    default:
        coast_render(&g_cfg->locations[loc], g_cfg->rain_km[loc]);
        return rain_poll(loc, for_view) ? RAIN_POLL_MS : RAIN_RETRY_MS;
    }
}

