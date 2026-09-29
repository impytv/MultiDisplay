#pragma once

#include <stdint.h>

#include "esp_lv_adapter.h"

/* The week screen: the next seven days of a location's forecast (see
 * week.c). Adapter lock held. */
lv_obj_t *week_build(lv_obj_t *screen);

/* Show location `loc`'s week (adapter lock held). */
void week_enter(int loc);

/* Refresh the forecast if stale and show it if the screen is still
 * `for_view`; the wait in ms before the next poll. Weather task. */
uint32_t week_poll(int loc, int for_view);
