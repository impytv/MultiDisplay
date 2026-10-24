/* The ISS screen: where the International Space Station is over the world
 * map, the last time round as a line behind it and the next as dots ahead,
 * the ground it can be seen from (above the horizon) as a ring around it,
 * day and night on the map, and the location chosen on the setup page.
 *
 * The orbit comes from the station's two-line elements (TLE) at Celestrak
 * (or ARISS, if Celestrak doesn't answer), fetched every 6 hours while the
 * screen is on show and kept in NVS, so a restart doesn't need them again
 * and the screen shows at once; it is worked out on the device (components/sgp4.c): no service is asked where it is. The map
 * (components/world.bin, scripts/build_world.py) is shaded for the sun's
 * position every few minutes into a 16-colour image of 800 x 400 - 160 KB,
 * held only while the screen is on show (and so counted as free by the
 * watchdog, main/watchdog.c). */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

#include "app.h"
#include "diag.h"
#include "draw.h"
#include "http_util.h"
#include "iss.h"
#include "sgp4.h"
#include "sun.h"
#include "watchdog.h"
#include "yr_client.h"

static const char *TAG = "iss";

#define ISS_TLE_MAX  1024
#define ISS_TIMEOUT_MS 30000              /* Celestrak sometimes takes 10-20 s to answer */
#define ISS_TLE_S    (6 * 60 * 60)        /* the elements are fetched again after this */
#define ISS_NVS      "iss"
#define ISS_KEEP_MAX 256                  /* the text kept in NVS: the name and two lines */
#define ISS_OLD_S    (3 * 24 * 3600)      /* elements older than this are said to be */
#define ISS_RETRY_MS 60000
#define ISS_POLL_MS  5000                 /* it moves about a pixel in this time */
#define ISS_SHADE_S  120                  /* the day and night redrawn after this */

#define MAP_W    800
#define MAP_H    400
#define MAP_TOP  80 /* under the title and the two lines of figures */
#define MAP_PAL  16
#define MAP_BYTES (MAP_PAL * 4 + MAP_W * MAP_H / 2)

#define TRACK_STEP_S 30  /* a point of the track every 30 s (about 4 px) */
#define TRACK_MAX    420 /* two orbits of 93 minutes, and 12 more minutes */
#define TRACK_AHEAD_S 600 /* worked out this far beyond what is shown, so it lasts */
#define FOOT_POINTS  72

#define C_PAST   0xFFFFFF
#define C_NEXT   0xFFD54A
#define C_ISS    0xFF3B30
#define C_FOOT   0xFFFFFF
#define C_STALE  0xE07000

/* The map: 0 sea .. 3 land per pixel, 2 bits each (scripts/build_world.py). */
extern const uint8_t world_bin_start[] asm("_binary_world_bin_start");
extern const uint8_t world_bin_end[] asm("_binary_world_bin_end");

static lv_obj_t *s_root, *s_canvas, *s_title, *s_line1, *s_line2, *s_age;
static const lv_font_t *s_small;

/* Held under the adapter lock, set by the weather task. */
static sgp4_t s_tle;
static bool s_have_tle;
static time_t s_tle_at; /* when the elements held were fetched (wall clock), 0 = never */

/* Where the elements come from: Celestrak, else ARISS's copy (the same
 * elements, from the amateur radio on the station). */
static const char *const TLE_URLS[] = {
    "https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE",
    "https://live.ariss.org/iss.txt",
};
static backoff_t s_fail;
static uint8_t *s_map;           /* palette, then 4-bit pixels; NULL while not on show */
static lv_image_dsc_t s_map_img;
static double s_shaded_at;       /* when the map was shaded for, 0 = not */
typedef struct {
    float lat, lon;
} geo_t;
static EXT_RAM_BSS_ATTR geo_t s_track[TRACK_MAX];
static EXT_RAM_BSS_ATTR geo_t s_next_track[TRACK_MAX];
static EXT_RAM_BSS_ATTR float s_col_cos[MAP_W];
static double s_track_t0;        /* time of s_track[0], 0 = none */
static int s_track_n;
static double s_period_s;        /* one orbit */
static double s_now;             /* the time drawn for */
static sgp4_ground_t s_here;     /* where it is then */
static bool s_here_ok;
static int s_drawn_x = -1, s_drawn_y = -1;

static int map_x(double lon)
{
    return (int)lround((lon + 180.0) * MAP_W / 360.0);
}

static int map_y(double lat)
{
    return MAP_TOP + (int)lround((90.0 - lat) * MAP_H / 180.0);
}

/* A colour between a and b, t of the way (0..1). */
static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        const float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        out |= (uint32_t)lroundf(ca + (cb - ca) * t) << sh;
    }
    return out;
}

/* The palette: index light * 4 + land, light 0 (night) .. 3 (day), land
 * 0 (sea) .. 3 (all land in the pixel). */
static void make_palette(uint8_t *pal)
{
    static const uint32_t SEA[2] = { 0x0B1828, 0x2B5C88 };  /* night, day */
    static const uint32_t LAND[2] = { 0x27321F, 0x8AA56B };
    for (int light = 0; light < 4; light++) {
        const uint32_t sea = mix(SEA[0], SEA[1], light / 3.0f), land = mix(LAND[0], LAND[1], light / 3.0f);
        for (int c = 0; c < 4; c++) {
            const uint32_t rgb = mix(sea, land, c / 3.0f);
            uint8_t *p = pal + 4 * (light * 4 + c); /* lv_color32_t: blue, green, red, alpha */
            p[0] = rgb & 0xFF;
            p[1] = (rgb >> 8) & 0xFF;
            p[2] = (rgb >> 16) & 0xFF;
            p[3] = 0xFF;
        }
    }
}

/* Shade the map for the sun at `t`: day, two steps of twilight (the sun
 * up to 6 and 12 degrees below the horizon), night. */
static void shade(double t)
{
    double slat, slon;
    sun_subsolar(t, &slat, &slon, NULL);
    const float rad = (float)(M_PI / 180.0);
    const float sd = sinf((float)slat * rad), cd = cosf((float)slat * rad);
    for (int x = 0; x < MAP_W; x++) {
        s_col_cos[x] = cosf((-180.0f + (x + 0.5f) * 360.0f / MAP_W - (float)slon) * rad);
    }
    const float up = -0.0145f, civil = -0.1045f, nautical = -0.2079f; /* sines of -0.833, -6, -12 degrees */
    uint8_t *px = s_map + MAP_PAL * 4;
    for (int y = 0; y < MAP_H; y++) {
        const float lat = (90.0f - (y + 0.5f) * 180.0f / MAP_H) * rad;
        const float a = sinf(lat) * sd, b = cosf(lat) * cd;
        const uint8_t *src = world_bin_start + y * (MAP_W / 4);
        uint8_t *dst = px + y * (MAP_W / 2);
        for (int x = 0; x < MAP_W; x += 2) {
            uint8_t two = 0;
            for (int k = 0; k < 2; k++) {
                const int xx = x + k;
                const float e = a + b * s_col_cos[xx];
                const int light = e > up ? 3 : e > civil ? 2 : e > nautical ? 1 : 0;
                const int land = (src[xx >> 2] >> (6 - 2 * (xx & 3))) & 3;
                two |= (uint8_t)((light * 4 + land) << (k ? 0 : 4));
            }
            dst[x >> 1] = two;
        }
    }
    s_shaded_at = t;
}

/* A line from a to b, the shorter way round: across the date line it is
 * drawn twice, leaving one edge and coming in at the other. */
static void geo_line(lv_layer_t *layer, geo_t a, geo_t b, int width, lv_color_t c, lv_opa_t opa)
{
    float blon = b.lon;
    if (blon - a.lon > 180.0f) {
        blon -= 360.0f;
    } else if (a.lon - blon > 180.0f) {
        blon += 360.0f;
    }
    const int x1 = map_x(a.lon), y1 = map_y(a.lat), x2 = map_x(blon), y2 = map_y(b.lat);
    const int shifts[2] = { 0, x2 < 0 ? MAP_W : (x2 > MAP_W ? -MAP_W : 0) };
    for (int i = 0; i < (shifts[1] ? 2 : 1); i++) {
        const int lo_x = (x1 < x2 ? x1 : x2) + shifts[i], hi_x = (x1 < x2 ? x2 : x1) + shifts[i];
        if (!draw_visible(layer, lo_x - width, (y1 < y2 ? y1 : y2) - width, hi_x + width, (y1 < y2 ? y2 : y1) + width)) {
            continue;
        }
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.p1.x = x1 + shifts[i];
        d.p1.y = y1;
        d.p2.x = x2 + shifts[i];
        d.p2.y = y2;
        d.width = width;
        d.color = c;
        d.opa = opa;
        d.round_start = d.round_end = 1;
        lv_draw_line(layer, &d);
    }
}

/* Text with a dark edge under it, to read on any part of the map. */
static void map_text(lv_layer_t *layer, const char *txt, int x, int y, lv_color_t c)
{
    const int h = lv_font_get_line_height(s_small);
    if (!draw_visible(layer, x - 1, y - 1, x + 300, y + h + 1)) {
        return;
    }
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = s_small;
    d.text = txt;
    d.text_local = 1;
    for (int i = 0; i < 2; i++) {
        d.color = i ? c : lv_color_black();
        d.opa = i ? LV_OPA_COVER : LV_OPA_70;
        lv_area_t a = { x + (i ? 0 : 1), y + (i ? 0 : 1), x + 300, y + h };
        lv_draw_label(layer, &d, &a);
    }
}

static void iss_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_map == NULL || s_shaded_at == 0) {
        return;
    }
    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = &s_map_img;
    lv_area_t a = { 0, MAP_TOP, MAP_W - 1, MAP_TOP + MAP_H - 1 };
    lv_draw_image(layer, &img, &a);

    /* Every 30 degrees, faintly. */
    const lv_color_t white = lv_color_white();
    for (int lon = -150; lon < 180; lon += 30) {
        const int x = map_x(lon);
        if (draw_visible(layer, x, MAP_TOP, x, MAP_TOP + MAP_H - 1)) {
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.p1.x = d.p2.x = x;
            d.p1.y = MAP_TOP;
            d.p2.y = MAP_TOP + MAP_H - 1;
            d.color = white;
            d.opa = lon == 0 ? LV_OPA_30 : LV_OPA_20;
            d.width = 1;
            lv_draw_line(layer, &d);
        }
    }
    for (int lat = -60; lat <= 60; lat += 30) {
        const int y = map_y(lat);
        if (draw_visible(layer, 0, y, MAP_W - 1, y)) {
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.p1.x = 0;
            d.p2.x = MAP_W - 1;
            d.p1.y = d.p2.y = y;
            d.color = white;
            d.opa = lat == 0 ? LV_OPA_30 : LV_OPA_20;
            d.width = 1;
            lv_draw_line(layer, &d);
        }
    }

    /* The place chosen on the setup page (g_cfg->iss_loc), with its name. */
    if (g_cfg->iss_loc > 0 && g_cfg->iss_loc <= g_cfg->location_count) {
        const app_location_t *loc = &g_cfg->locations[g_cfg->iss_loc - 1];
        const int x = map_x(atof(loc->lon)), y = map_y(atof(loc->lat));
        draw_dot(layer, x, y, 4, lv_color_black());
        draw_dot(layer, x, y, 3, white);
        map_text(layer, loc->name, x + 7, y - lv_font_get_line_height(s_small) / 2, white);
    }

    if (!s_here_ok || s_track_n == 0) {
        return;
    }
    const geo_t here = { (float)s_here.lat, (float)s_here.lon };

    /* Where it can be seen from: the ground within the angle where it is
     * on the horizon, acos(R / (R + h)) from the point under it. */
    const double r_km = 6371.0, rad = M_PI / 180.0;
    const double ang = acos(r_km / (r_km + s_here.alt_km));
    const double la = s_here.lat * rad, lo = s_here.lon * rad;
    geo_t prev = { 0, 0 };
    for (int i = 0; i <= FOOT_POINTS; i++) {
        const double brg = 2.0 * M_PI * i / FOOT_POINTS;
        const double la2 = asin(sin(la) * cos(ang) + cos(la) * sin(ang) * cos(brg));
        const double lo2 = lo + atan2(sin(brg) * sin(ang) * cos(la), cos(ang) - sin(la) * sin(la2));
        const geo_t p = { (float)(la2 / rad), (float)remainder(lo2 / rad, 360.0) };
        if (i > 0) {
            geo_line(layer, prev, p, 1, lv_color_hex(C_FOOT), LV_OPA_60);
        }
        prev = p;
    }

    /* The last time round as a line up to it, the next as dots. */
    const double t0 = s_now - s_period_s, t1 = s_now + s_period_s;
    bool have_prev = false;
    for (int i = 0; i < s_track_n; i++) {
        const double t = s_track_t0 + (double)i * TRACK_STEP_S;
        if (t < t0) {
            continue;
        }
        if (t > s_now) {
            break;
        }
        if (have_prev) {
            geo_line(layer, prev, s_track[i], 2, lv_color_hex(C_PAST), LV_OPA_80);
        }
        prev = s_track[i];
        have_prev = true;
    }
    if (have_prev) {
        geo_line(layer, prev, here, 2, lv_color_hex(C_PAST), LV_OPA_80);
    }
    for (int i = 0; i < s_track_n; i++) {
        const double t = s_track_t0 + (double)i * TRACK_STEP_S;
        if (t <= s_now + TRACK_STEP_S / 2) {
            continue;
        }
        if (t > t1) {
            break;
        }
        draw_dot(layer, map_x(s_track[i].lon), map_y(s_track[i].lat), 2, lv_color_hex(C_NEXT));
    }

    /* The station. */
    const int x = map_x(here.lon), y = map_y(here.lat);
    draw_dot(layer, x, y, 10, lv_color_black());
    draw_dot(layer, x, y, 9, white);
    draw_dot(layer, x, y, 6, lv_color_hex(C_ISS));
    s_drawn_x = x;
    s_drawn_y = y;

    /* What the lines are, bottom left. */
    const int lh = lv_font_get_line_height(s_small);
    const int ly = MAP_TOP + MAP_H - lh - 6;
    if (draw_visible(layer, 0, ly - 4, 400, MAP_TOP + MAP_H - 1)) {
        draw_line(layer, 10, ly + lh / 2, 34, ly + lh / 2, 2, lv_color_hex(C_PAST));
        map_text(layer, "Forrige runde", 40, ly, white);
        lv_point_t sz;
        lv_text_get_size(&sz, "Forrige runde", s_small, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int x2 = 40 + sz.x + 20;
        for (int k = 0; k < 4; k++) {
            draw_dot(layer, x2 + 2 + 6 * k, ly + lh / 2, 2, lv_color_hex(C_NEXT));
        }
        map_text(layer, "Neste runde", x2 + 30, ly, white);
    }
}

/* "59,9° N, 10,8° Ø": degrees to a tenth, with a decimal comma. */
static void lat_lon_text(char *buf, size_t len, double lat, double lon)
{
    const int la = (int)lround(fabs(lat) * 10), lo = (int)lround(fabs(lon) * 10);
    snprintf(buf, len, "%d,%d\xC2\xB0 %s, %d,%d\xC2\xB0 %s", la / 10, la % 10, lat < 0 ? "S" : "N", lo / 10, lo % 10,
             lon < 0 ? "V" : "\xC3\x98");
}

/* Whether it is in the earth's shadow at `t` (a cylinder behind the earth
 * - near enough for a station 400 km up). */
static bool in_shadow(double t)
{
    double r[3], v[3], sun[3], slat, slon;
    if (!sgp4_pos(&s_tle, t, r, v)) {
        return false;
    }
    sun_subsolar(t, &slat, &slon, sun);
    const double along = r[0] * sun[0] + r[1] * sun[1] + r[2] * sun[2];
    if (along > 0) {
        return false; /* on the sun's side */
    }
    const double off2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] - along * along;
    return off2 < 6371.0 * 6371.0;
}

/* The labels for what is held (adapter lock held). */
static void iss_show(void)
{
    if (!s_have_tle) {
        lv_label_set_text(s_line1, "");
        lv_label_set_text(s_line2, "");
        lv_label_set_text(s_age, "");
        lv_label_set_text(g_status_label, s_fail.count ? "Kunne ikke hente banen til romstasjonen. Pr\xC3\xB8ver igjen..."
                                                       : "Henter banen til romstasjonen...");
        return;
    }
    if (s_map == NULL) {
        lv_label_set_text(g_status_label, "Ikke nok minne til kartet.");
    } else {
        lv_label_set_text(g_status_label, "");
    }
    if (s_here_ok) {
        char pos[48];
        lat_lon_text(pos, sizeof(pos), s_here.lat, s_here.lon);
        const int kmh = (int)lround(s_here.speed_kmh / 10.0) * 10;
        lv_label_set_text_fmt(s_line1, "%d km over bakken \xC2\xB7 %d %03d km/t", (int)lround(s_here.alt_km), kmh / 1000,
                              kmh % 1000);
        lv_label_set_text_fmt(s_line2, "Over %s \xC2\xB7 %s", pos, in_shadow(s_now) ? "i jordskyggen" : "i sollys");
    }
    const time_t now = time(NULL);
    if (now - (time_t)s_tle.epoch > ISS_OLD_S) {
        struct tm lt;
        const time_t ep = (time_t)s_tle.epoch;
        localtime_r(&ep, &lt);
        lv_label_set_text_fmt(s_age, "Banen er fra %d.%d.", lt.tm_mday, lt.tm_mon + 1);
    } else {
        lv_label_set_text(s_age, "");
    }
}

lv_obj_t *iss_build(lv_obj_t *screen)
{
    assert(world_bin_end - world_bin_start == MAP_W * MAP_H / 4);
    s_root = screen_root_create(screen);
    s_small = load_font(15, false);

    s_canvas = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_set_size(s_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, iss_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, g_font_large, 0);
    lv_obj_set_pos(s_title, 12, 4);
    lv_label_set_text(s_title, "Romstasjonen ISS");

    s_line1 = lv_label_create(s_root);
    lv_obj_align(s_line1, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_label_set_text(s_line1, "");

    s_line2 = lv_label_create(s_root);
    lv_obj_set_pos(s_line2, 12, MAP_TOP - lv_font_get_line_height(g_font_body) - 6);
    lv_label_set_text(s_line2, "");

    s_age = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_age, lv_color_hex(C_STALE), 0);
    lv_obj_align(s_age, LV_ALIGN_TOP_RIGHT, -12, MAP_TOP - lv_font_get_line_height(g_font_body) - 6);
    lv_label_set_text(s_age, "");
    return s_root;
}

void iss_enter(void)
{
    s_drawn_x = s_drawn_y = -1;
    iss_show();
    lv_obj_invalidate(s_canvas);
}

void iss_release(void)
{
    if (s_map == NULL || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    free(s_map);
    s_map = NULL;
    s_shaded_at = 0;
    esp_lv_adapter_unlock();
    wd_psram_held(-MAP_BYTES);
}

/* The track from a little before one orbit ago to a little more than one
 * orbit ahead of `now`, on whole steps, into s_next_track. */
static int work_out_track(double now, double *t0)
{
    *t0 = floor((now - s_period_s) / TRACK_STEP_S) * TRACK_STEP_S - TRACK_STEP_S;
    int n = 0;
    for (; n < TRACK_MAX; n++) {
        const double t = *t0 + (double)n * TRACK_STEP_S;
        if (t > now + s_period_s + TRACK_AHEAD_S) {
            break;
        }
        sgp4_ground_t g;
        if (!sgp4_ground(&s_tle, t, &g)) {
            break;
        }
        s_next_track[n] = (geo_t){ (float)g.lat, (float)g.lon };
    }
    return n;
}

/* Take `text` (a TLE) as the elements held; false if it isn't one. */
static bool take_tle(const char *text, time_t fetched)
{
    sgp4_t tle;
    if (!sgp4_parse_text(text, &tle)) {
        return false;
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        s_tle = tle;
        s_have_tle = true;
        s_period_s = 2.0 * M_PI / tle.no_unkozai * 60.0;
        s_track_t0 = 0; /* worked out again from these */
        s_tle_at = fetched;
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "Elements from %.0f, %.1f minutes round", tle.epoch, s_period_s / 60.0);
    return true;
}

/* The elements kept from the last fetch, if any (once, at the first poll). */
static void load_tle(void)
{
    nvs_handle_t h;
    if (nvs_open(ISS_NVS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char text[ISS_KEEP_MAX];
    size_t len = sizeof(text);
    int64_t at = 0;
    if (nvs_get_str(h, "tle", text, &len) == ESP_OK && nvs_get_i64(h, "at", &at) == ESP_OK) {
        take_tle(text, (time_t)at);
    }
    nvs_close(h);
}

static void save_tle(const char *text, time_t fetched)
{
    nvs_handle_t h;
    if (strlen(text) >= ISS_KEEP_MAX || nvs_open(ISS_NVS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_str(h, "tle", text) != ESP_OK || nvs_set_i64(h, "at", (int64_t)fetched) != ESP_OK ||
        nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "Couldn't keep the elements");
    }
    nvs_close(h);
}

/* Fetch the elements, from the first source that gives them; false if
 * none did. */
static bool fetch_tle(void)
{
    esp_err_t err = ESP_FAIL;
    for (size_t k = 0; k < sizeof(TLE_URLS) / sizeof(TLE_URLS[0]); k++) {
        char *body = NULL;
        err = http_get_body(TLE_URLS[k], yr_client_user_agent(), ISS_TIMEOUT_MS, ISS_TLE_MAX, &body, TAG);
        const time_t now = time(NULL);
        if (err == ESP_OK && take_tle(body, now)) {
            save_tle(body, now);
            free(body);
            diag_ok(DIAG_ISS);
            backoff_ok(&s_fail);
            return true;
        }
        free(body);
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGW(TAG, "No elements for the ISS from %s: %s", TLE_URLS[k], esp_err_to_name(err));
    }
    diag_fail(DIAG_ISS, err);
    backoff_fail(&s_fail);
    return false;
}

uint32_t iss_poll(int for_view)
{
    const time_t now_s = time(NULL);
    if (now_s <= PLAUSIBLE_EPOCH_S) {
        return ISS_RETRY_MS / 6; /* where it is needs the clock */
    }
    static bool loaded;
    if (!loaded) {
        loaded = true;
        load_tle();
    }
    if ((s_tle_at == 0 || now_s - s_tle_at >= ISS_TLE_S || now_s < s_tle_at) &&
        !backoff_waiting(&s_fail, ISS_RETRY_MS)) {
        fetch_tle();
    }
    if (!s_have_tle) {
        if (lock_for_view(for_view)) {
            iss_show();
            esp_lv_adapter_unlock();
        }
        return backoff_ms(&s_fail, ISS_RETRY_MS);
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    const double now = (double)tv.tv_sec + tv.tv_usec / 1e6;

    /* The track, when what is held runs out (or the elements changed). */
    double t0 = s_track_t0;
    int n = 0;
    if (s_track_t0 == 0 || s_track_t0 + (double)(s_track_n - 1) * TRACK_STEP_S < now + s_period_s ||
        s_track_t0 > now - s_period_s) {
        n = work_out_track(now, &t0);
    }
    sgp4_ground_t here;
    const bool here_ok = sgp4_ground(&s_tle, now, &here);

    if (s_map == NULL) {
        uint8_t *map = heap_caps_malloc(MAP_BYTES, MALLOC_CAP_SPIRAM);
        if (map == NULL) {
            ESP_LOGE(TAG, "Out of memory for the map");
        } else if (esp_lv_adapter_lock(-1) == ESP_OK) {
            s_map = map;
            make_palette(s_map);
            s_map_img = (lv_image_dsc_t){
                .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I4, .w = MAP_W, .h = MAP_H,
                            .stride = MAP_W / 2 },
                .data = s_map,
                .data_size = MAP_BYTES,
            };
            esp_lv_adapter_unlock();
            wd_psram_held(MAP_BYTES); /* given back when another screen shows */
        } else {
            free(map);
        }
    }

    if (lock_for_view(for_view)) {
        bool redraw = false;
        if (s_map != NULL && (s_shaded_at == 0 || now - s_shaded_at > ISS_SHADE_S)) {
            shade(now);
            redraw = true;
        }
        if (n > 0) {
            memcpy(s_track, s_next_track, (size_t)n * sizeof(s_track[0]));
            s_track_n = n;
            s_track_t0 = t0;
            redraw = true;
        }
        s_now = now;
        s_here = here;
        s_here_ok = here_ok;
        if (map_x(here.lon) != s_drawn_x || map_y(here.lat) != s_drawn_y) {
            redraw = true;
        }
        iss_show();
        if (redraw) {
            lv_obj_invalidate(s_canvas);
        }
        esp_lv_adapter_unlock();
    }
    return ISS_POLL_MS;
}
