#ifndef _HTTP_UTIL_H_
#define _HTTP_UTIL_H_

/* What the API clients (yr, met_alerts, adsb, ais, rain, entur) share. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* A response body collected in PSRAM - TLS already fights for internal DRAM
 * while it streams in. Always NUL-terminated once anything is in it. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t max; /* refuse a body longer than this (a runaway server) */
} http_buf_t;

/* Empty it for the next response, keeping the allocation. */
void http_buf_reset(http_buf_t *b);

/* Free the allocation too. */
void http_buf_free(http_buf_t *b);

/* Append `len` bytes (from an HTTP_EVENT_ON_DATA); ESP_FAIL, logged under
 * `tag`, if that would pass b->max or memory runs out. */
esp_err_t http_buf_append(http_buf_t *b, const void *data, size_t len, const char *tag);

/* One GET of `url` with no certificate check (see yr_client.c): on HTTP 200
 * with a body, ESP_OK and the body in *body, NUL-terminated, in PSRAM, for
 * the caller to free(). Failures are logged under `tag`. */
esp_err_t http_get_body(const char *url, const char *user_agent, int timeout_ms, size_t max, char **body,
                        const char *tag);

/* After a request failed on a kept-alive client: whether to try it once
 * more on a fresh connection. Only if the connection was a reused one (the
 * server may have closed it while idle) and it failed quickly - one that
 * timed out isn't repeated, which would only double the wait. `started_us`
 * is esp_timer_get_time() from before the request. */
bool http_retry_worthwhile(bool reused, int64_t started_us);

/* Have cJSON allocate from PSRAM: a parse is thousands of small nodes, all
 * under the 1 KB above which plain malloc would use PSRAM by itself. Global
 * to cJSON; call before any parse. */
void json_use_psram(void);

/* Epoch seconds for an ISO 8601 time, "2026-09-27T20:31:00+02:00",
 * "...:00.123Z" or "...:00Z"; a missing zone is taken as UTC. 0 if it
 * doesn't parse. */
int64_t iso8601_to_epoch(const char *s);

#endif
