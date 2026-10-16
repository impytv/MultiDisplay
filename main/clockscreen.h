#pragma once

#include "esp_lv_adapter.h"

/* The clock screen: the time in large digits, the date and the
 * temperature indoors (see clockscreen.c). Adapter lock held. */
lv_obj_t *clockscreen_build(lv_obj_t *screen);

/* Show it (adapter lock held). */
void clockscreen_enter(void);

/* Nothing to fetch: the wait in ms before the next poll. Weather task. */
uint32_t clockscreen_poll(int for_view);
