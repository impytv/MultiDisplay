#ifndef _SONOS_H_
#define _SONOS_H_

/* The Sonos screen (see sonos.c). */

#include <stdbool.h>
#include <stdint.h>

#include "esp_lv_adapter.h"

lv_obj_t *sonos_build(lv_obj_t *screen);
void sonos_enter(void);
/* Weather task: send a button's command, fetch what plays; the ms to the
 * next poll. */
uint32_t sonos_poll(int for_view);
/* Weather task, on another screen: give back the covers' memory. */
void sonos_release(void);
/* A touch at x, y (LVGL context): true if it was one of the screen's
 * buttons, or beside them. */
bool sonos_tap(int x, int y);

#endif
