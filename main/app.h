#ifndef _APP_H_
#define _APP_H_

/* What the screens (weather.c, radar.c, departures.c) share with main.c. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_lv_adapter.h"
#include "app_config.h"

/* Treat the clock as synced once it reads past this (2023-01-01 UTC) -
 * comfortably below "now" for the life of this project, comfortably above
 * the unsynced epoch. */
#define PLAUSIBLE_EPOCH_S 1672531200

/* The kinds of screen a tap cycles through (see build_stops in main.c). */
typedef enum {
    STOP_OVERVIEW = 0, STOP_WEATHER = 1, STOP_RADAR = 2, STOP_SHIPS = 3, STOP_RAIN = 4, STOP_DEPARTURES = 5,
    STOP_AIR = 6, STOP_WEEK = 7, STOP_TIDE = 8, STOP_CALENDAR = 9
} stop_kind_t;

/* Runtime settings (WiFi + locations), from NVS via the setup page or the
 * compiled-in defaults. Loaded once in app_main, into PSRAM. */
extern app_config_t *g_cfg;

/* Body and title fonts, at the sizes set on the setup page. */
extern const lv_font_t *g_font_body;
extern const lv_font_t *g_font_large;

/* Loading/error text, centred over whichever screen is shown. */
extern lv_obj_t *g_status_label;

/* Index of the screen on show (see main.c); switched from the LVGL task. */
extern volatile int g_view_index;

/* A FreeType font of the bundled Montserrat at `px`. */
const lv_font_t *load_font(uint8_t px, bool bold);

/* Take the adapter lock to show what was fetched for screen `for_view`;
 * false, without the lock, if the screen has moved on. Checked under the
 * lock, as taps switch screens from the LVGL task. */
bool lock_for_view(int for_view);

/* When some data was fetched, so a screen coming back can tell whether its
 * last data is still worth showing. */
typedef struct {
    bool valid;
    uint32_t tick; /* lv_tick_get() when fetched */
    time_t when;   /* wall clock when fetched, 0 if not synced yet */
} fetch_stamp_t;

static inline void stamp_now(fetch_stamp_t *st)
{
    const time_t now = time(NULL);
    st->valid = true;
    st->tick = lv_tick_get();
    st->when = (now > PLAUSIBLE_EPOCH_S) ? now : 0;
}

static inline bool stamp_fresh(const fetch_stamp_t *st, uint32_t max_ms)
{
    return st->valid && lv_tick_get() - st->tick < max_ms;
}

/* A full-screen, transparent container for one screen, hidden until shown. */
lv_obj_t *screen_root_create(lv_obj_t *screen);

#endif
