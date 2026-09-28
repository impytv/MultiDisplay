#ifndef _WEATHER_H_
#define _WEATHER_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_lv_adapter.h"

/* The per-location forecast and alert caches. Before building. */
void weather_init(void);

/* The detail screen and the overview table (adapter lock held). */
lv_obj_t *weather_build(lv_obj_t *screen);
lv_obj_t *overview_build(lv_obj_t *screen);

/* Show location `loc`'s weather screen - its last drawing if recent - or the
 * overview (adapter lock held). */
void weather_enter(int loc);
void overview_enter(void);

/* Refresh what's stale and show it if the screen is still `for_view` (the
 * overview, or location `sel`'s screen, its forecast refetched if
 * `refetch_sel`); the wait in ms before the next poll, 0 if the screen
 * changed. Weather task. */
uint32_t weather_poll(bool overview, int sel, bool refetch_sel, int for_view);

#endif
