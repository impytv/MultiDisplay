#pragma once

/* The board's microSD (TF) card, as a FAT file system at SD_CARD_ROOT
 * (see sd_card.c). Mounted when first needed, and again after the card has
 * been out. Everything that touches the card takes the lock first: the
 * picture screen (weather task) and the web pages (web server) share it. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SD_CARD_ROOT "/sd"

/* Once at start, before any task uses the card (app_main). */
void sd_card_init(void);

void sd_card_lock(void);
void sd_card_unlock(void);

/* With the lock held: mount the card if it isn't. ESP_OK when it is
 * mounted; ESP_ERR_NOT_FOUND with no card (or one it can't read), tried
 * again at most every few seconds. */
esp_err_t sd_card_ready(void);

/* With the lock held: a read or write on the card failed - perhaps it was
 * taken out. Unmounted, to be mounted again next time. */
void sd_card_failed(void);

/* With the lock held and the card mounted: its size and free space in
 * bytes. */
esp_err_t sd_card_space(uint64_t *total, uint64_t *free_bytes);

/* With the lock held and the card mounted: FatFs's name for its drive
 * ("0:"), for f_opendir() and the like. */
const char *sd_card_drive(void);

/* With the lock held, after sd_card_ready() failed: NULL if no card
 * answered, else what the one in the slot holds instead of a file system
 * the display reads - "ntfs", "uformatert" (no file system at all), or
 * "ukjent" / "gpt" (something else). */
const char *sd_card_unreadable(void);
