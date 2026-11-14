#pragma once

/* Downloads kept on the memory card, when there is one, so they needn't be
 * fetched again: the rain radar's frames, and covers and logos for the
 * music screens. The card is optional - with none, or one that can't be
 * read or is full, every get misses and every put does nothing, and the
 * callers fetch as they would without. In CACHE/<kind>/ on the card, apart
 * from the pictures (BILDER); each kind has a cap on files and bytes, and
 * the oldest go first. Safe to delete at any time. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "http_util.h"

/* How long a cover or logo is kept before it is fetched again (one that
 * changed comes with a new address anyway, as far as the screens go). */
#define SD_CACHE_COVER_AGE_S (30 * 24 * 3600)
/* The update site's radio logos (KEY.jpg, and the pickers' sheet of them,
 * tiles.bin), which can be made again at the same address: sooner. */
#define SD_CACHE_LOGO_AGE_S (7 * 24 * 3600)

typedef enum {
    SD_CACHE_RAIN,  /* the rain radar's frames as rain levels (rain_client.c) */
    SD_CACHE_COVER, /* covers and logos, as fetched (music screens) */
    SD_CACHE_KINDS
} sd_cache_kind_t;

/* The data kept for `key`, if no older than max_age_s (0 = any age), into
 * a new PSRAM buffer with a '\0' after it (*data, for free()), at most
 * `max` bytes. False with no card, nothing kept, or a file that doesn't
 * check out. */
bool sd_cache_get(sd_cache_kind_t kind, const char *key, uint32_t max_age_s, size_t max, uint8_t **data,
                  size_t *len);

/* The data kept for `key` into buf, if it is exactly `len` bytes. */
bool sd_cache_get_into(sd_cache_kind_t kind, const char *key, uint32_t max_age_s, void *buf, size_t len);

/* Keep `len` bytes for `key`, replacing what was kept for it; nothing
 * happens without a card or room on it. */
void sd_cache_put(sd_cache_kind_t kind, const char *key, const void *data, size_t len);

/* Forget what is kept for `key`. */
void sd_cache_drop(sd_cache_kind_t kind, const char *key);

/* http_get_buf() of `url` through the cache: the kept copy if there is
 * one no older than max_age_s, else fetched and kept. *from_card (may be
 * NULL) says which. b->max limits both. */
esp_err_t sd_cache_http_get(sd_cache_kind_t kind, const char *url, uint32_t max_age_s, const char *user_agent,
                            int timeout_ms, bool verify, http_buf_t *b, const char *tag, bool *from_card);

/* The same without looking on the card first (the caller knows it isn't
 * there, or wants it fresh): fetched, and kept if it is a picture (or, for
 * SD_CACHE_COVER, a sheet of logos). */
esp_err_t sd_cache_http_fetch(sd_cache_kind_t kind, const char *url, const char *user_agent, int timeout_ms,
                              bool verify, http_buf_t *b, const char *tag);
