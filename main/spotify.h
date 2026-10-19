#ifndef _SPOTIFY_H_
#define _SPOTIFY_H_

/* The Spotify screen (see spotify.c). */

#include <stdbool.h>
#include <stdint.h>

#include "esp_lv_adapter.h"

lv_obj_t *spotify_build(lv_obj_t *screen);
void spotify_enter(void);
/* Weather task: send a button's command, fetch what plays; the ms to the
 * next poll. */
uint32_t spotify_poll(int for_view);
/* Weather task, on another screen: give back the cover's memory. */
void spotify_release(void);
/* A touch at x, y (LVGL context): true if it was one of the screen's
 * buttons, or beside them. */
bool spotify_tap(int x, int y);
/* The login's pages on the display's web server (/spotify/...). */
void spotify_add_handlers(void);

#endif
