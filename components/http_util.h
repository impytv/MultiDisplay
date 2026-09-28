#ifndef _HTTP_UTIL_H_
#define _HTTP_UTIL_H_

/* What the API clients (yr, met_alerts, adsb, ais, rain, entur) share. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

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

/* MET's terms (api.met.no) ask clients to wait until a response's Expires
 * before asking again, and then to ask with If-Modified-Since. One of these
 * per resource and location, kept with the data it describes. */
typedef struct {
    char last_modified[40]; /* the Last-Modified sent back as If-Modified-Since, "" = none */
    int64_t expires;        /* epoch seconds of Expires, 0 = unknown */
} http_cache_t;

/* What http_get_body_cached returns when the data held is still current:
 * no body. */
#define HTTP_NOT_MODIFIED 0x7f01

/* http_get_body for a resource whose last response the caller still holds,
 * described by `cache`: HTTP_NOT_MODIFIED without asking before it expires
 * (unless `ignore_expires`, e.g. for a tap), or when the server answers 304.
 * Otherwise as http_get_body, and `cache` describes the new body. */
esp_err_t http_get_body_cached(const char *url, const char *user_agent, int timeout_ms, size_t max, char **body,
                               const char *tag, http_cache_t *cache, bool ignore_expires);

/* When the clock was set from a response's Date header (see
 * http_get_body_cached): only while it wasn't set yet, so NTP stays in
 * charge once it answers. 0 = never. */
time_t http_clock_set_at(void);

/* Epoch seconds for an HTTP date ("Mon, 28 Sep 2026 15:12:31 GMT"), 0 if it
 * doesn't parse. */
int64_t http_date_to_epoch(const char *s);

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
