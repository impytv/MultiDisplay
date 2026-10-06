#ifndef _SAT_CLIENT_H_
#define _SAT_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "http_util.h"
#include "sat_util.h"

/* Handed each row of the image, top to bottom: SAT_IMG_W pixels of RGB888. */
typedef void (*sat_row_fn)(int y, const uint8_t *rgb, void *ctx);

/**
 * Fetch MET Norway's latest satellite image of Europe - infrared, or in
 * visible light (black where it is night) - and hand it
 * to `row` as it arrives, rather than holding the whole ~1 MB file. `cache`
 * (see http_util.h) is the image last fetched: HTTP_NOT_MODIFIED, with no
 * rows, if it's still the latest. ESP_OK once every row has been handed
 * on; `*taken` is when the image was taken (0 if unknown). On failure,
 * `*rows` says how many rows `row` was given before it - some of the old
 * image may then be overwritten.
 */
esp_err_t sat_client_fetch(bool visible, http_cache_t *cache, sat_row_fn row, void *ctx, time_t *taken, int *rows);

#endif
