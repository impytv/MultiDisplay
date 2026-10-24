#pragma once

#include "esp_lv_adapter.h"

/* The ISS screen (see iss.c). */
lv_obj_t *iss_build(lv_obj_t *screen);
/* Show what is held for it (adapter lock held). */
void iss_enter(void);
/* From the weather task while it is on show: fetch the orbit when due and
 * move the station on; returns the ms until the next call. */
uint32_t iss_poll(int for_view);
/* Free the map while another screen is on show (weather task). */
void iss_release(void);
