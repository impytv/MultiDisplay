#pragma once

#include "esp_lv_adapter.h"

/* The picture screen (see pictures.c): pictures from the microSD card, at
 * random. */
lv_obj_t *pictures_build(lv_obj_t *screen);
/* Show what is held for it (adapter lock held). */
void pictures_enter(void);
/* From the weather task while it is on show: the next picture when due;
 * returns the ms until the next call. */
uint32_t pictures_poll(int for_view);
/* Give back the picture's memory while another screen is on show (weather
 * task). */
void pictures_release(void);
/* The /bilder page and what it asks for (web server). */
void pictures_add_handlers(void);
