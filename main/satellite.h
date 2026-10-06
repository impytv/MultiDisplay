#pragma once

#include "esp_lv_adapter.h"

/* The satellite screen: MET Norway's infrared image of Europe, and close-ups
 * of it around locations (see satellite.c). Adapter lock held. */
lv_obj_t *satellite_build(lv_obj_t *screen);

/* Show Europe (loc < 0) or the close-up around location `loc`, with the
 * last image if there is one (adapter lock held). */
void satellite_enter(int loc);

/* Fetch if a new image is due and show it if the screen is still
 * `for_view`; the wait in ms before the next poll. Weather task. */
uint32_t satellite_poll(int loc, int for_view);
