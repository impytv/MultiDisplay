#pragma once

#include <stdint.h>

#include "esp_lv_adapter.h"

/* The air screen: air quality and pollen for a location (see air.c).
 * Adapter lock held. */
lv_obj_t *air_build(lv_obj_t *screen);

/* Show location `loc`'s air screen, with its last data if recent (adapter
 * lock held). */
void air_enter(int loc);

/* Fetch what is due for `loc` and show it if the screen is still `for_view`;
 * the wait in ms before the next poll. Weather task. */
uint32_t air_poll(int loc, int for_view);
