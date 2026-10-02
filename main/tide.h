#pragma once

#include "esp_lv_adapter.h"

/* The tide screen: water level for a location (see tide.c). Adapter lock
 * held. */
lv_obj_t *tide_build(lv_obj_t *screen);

/* Show location `loc`'s tide screen, with its last data if recent (adapter
 * lock held). */
void tide_enter(int loc);

/* Fetch what is due for `loc` and show it if the screen is still `for_view`;
 * the wait in ms before the next poll. Weather task. */
uint32_t tide_poll(int loc, int for_view);
