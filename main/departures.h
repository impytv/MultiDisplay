#ifndef _DEPARTURES_H_
#define _DEPARTURES_H_

#include <stdint.h>

#include "esp_lv_adapter.h"

/* The departure board's screen, and the per-location caches. Adapter lock
 * held. */
lv_obj_t *departures_build(lv_obj_t *screen);

/* Show location `loc`'s board, with its last departures if recent (adapter
 * lock held). */
void departures_enter(int loc);

/* Fetch and show `loc`'s departures if the screen is still `for_view`; the
 * wait in ms before the next poll. Weather task. */
uint32_t departures_poll(int loc, int for_view);

#endif
