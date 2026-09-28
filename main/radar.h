#ifndef _RADAR_H_
#define _RADAR_H_

#include <stdint.h>

#include "esp_lv_adapter.h"

/* The radar screen - aircraft, ship traffic or rain around a location - and
 * the per-location caches. Adapter lock held. */
lv_obj_t *radar_build(lv_obj_t *screen);

/* Show location `loc`'s screen of stop kind `kind` (STOP_RADAR, STOP_SHIPS
 * or STOP_RAIN), with its last data if recent (adapter lock held). */
void radar_enter(int kind, int loc);

/* Fetch and show it if the screen is still `for_view`; the wait in ms
 * before the next poll. Weather task. */
uint32_t radar_poll(int kind, int loc, int for_view);

/* The screen's text colour (0xRRGGBB), for text laid over it. */
uint32_t radar_status_colour(void);

#endif
