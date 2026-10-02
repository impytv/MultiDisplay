#pragma once

#include "esp_lv_adapter.h"

/* The calendar screen: the next two weeks from up to three calendars (see
 * calendar.c). Adapter lock held. */
lv_obj_t *calendar_build(lv_obj_t *screen);

/* Show it, with its last data if recent (adapter lock held). */
void calendar_enter(void);

/* Fetch if due and show it if the screen is still `for_view`; the wait in
 * ms before the next poll. Weather task. */
uint32_t calendar_poll(int for_view);
