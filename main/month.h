#pragma once

#include "esp_lv_adapter.h"

/* The month calendar: five weeks from this one, with week numbers and the
 * holidays in red (see month.c). Adapter lock held. */
lv_obj_t *month_build(lv_obj_t *screen);

/* Show it (adapter lock held). */
void month_enter(void);

/* A tap at x, y while it is on show: true if it hit one of its buttons
 * (and was acted on), false for the tap to switch screens as usual. LVGL
 * context. */
bool month_tap(int x, int y);

/* Show it if the screen is still `for_view`; the wait in ms before the next
 * poll. Weather task. */
uint32_t month_poll(int for_view);
