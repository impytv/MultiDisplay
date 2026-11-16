#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_lv_adapter.h"
#include "sd_card.h"

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

/* For sharing (share.c). */

/* The card's folder of pictures. */
#define PICTURES_DIR SD_CARD_ROOT "/BILDER"
/* A picture from the page or another display, at most. */
#define PICTURES_UPLOAD_MAX (1024 * 1024)
/* A JPEG copied onto the card from a PC, at most. */
#define PICTURES_FILE_MAX (1536 * 1024)
/* Pictures on the card, at most (the rest aren't shown). */
#define PICTURES_MAX 1000

/* Whether `len` bytes can be taken from PSRAM for a moment and leave
 * enough for the other tasks. */
bool pictures_psram_room(size_t len);
/* Whether `n` is a picture's name the page may ask for. */
bool pictures_name_ok(const char *n);
/* File `name` from the card into a new PSRAM buffer, for heap_caps_free;
 * NULL if it can't be (SD lock held). */
uint8_t *pictures_read(const char *name, size_t *len);
/* `len` bytes saved as `name` (written under another name, then renamed, so
 * the screen never shows half a picture). False if not (SD lock held). */
bool pictures_save(const char *name, const uint8_t *data, size_t len);
/* Pictures were added to the card or deleted from it: listed again. */
void pictures_changed(void);
