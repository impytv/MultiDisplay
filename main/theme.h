#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* The colour theme every screen is drawn in (see theme.c): light or dark,
 * and in either the Christmas decorations - small red tree ornaments
 * hanging from the top edge, behind the screen's own text, and red
 * buttons. Decided once at boot, since the screens are built once. */

/* Decide the theme from the settings and the date. Call before the screens
 * are built. */
void theme_init(void);

/* Whether the screens use the dark layout. */
bool theme_dark(void);
/* Whether they have the Christmas decorations. */
bool theme_christmas(void);

/* A colour for the theme: `light` in the light theme, `dark` in the dark
 * one. */
uint32_t theme_hex(uint32_t dark, uint32_t light);
lv_color_t theme_color(uint32_t dark, uint32_t light);
/* The same for a screen that may be dark whatever the theme ("Alltid
 * mørkt"): `dark` says whether it is. */
lv_color_t theme_color_if(bool dark, uint32_t dark_hex, uint32_t light_hex);

/* Set LVGL's theme. LVGL context. */
void theme_apply(lv_obj_t *screen);

/* Hang the ornaments on a screen's root, to be called before anything else
 * is put on it so that everything else is drawn over them. No-op without
 * the decorations. LVGL context. */
void theme_decorate(lv_obj_t *root);

/* Once a second with the clock set: true when the date calls for another
 * theme than the screens were built in (the season began or ended while the
 * clock wasn't known, e.g. after a power cut). Remembers the new one, so
 * the caller restarts once. */
bool theme_season_changed(void);
