#ifndef _AURORA_CLIENT_H_
#define _AURORA_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "http_util.h"

/* Yr's aurora forecast for a place, an hour at a time for about the next
 * three days: what yr.no shows under "Nordlys". It is Yr's website API
 * (www.yr.no/api/v0/locations/LAT,LON/auroraforecast), not a documented
 * one: it may change without notice, and the weather screen then simply
 * shows no aurora. */

#define AURORA_MAX_HOURS 80

/* An hour worth looking up for: Yr's aurora value at least this (it goes
 * with Kp where the oval passes: about 0.33 at Kp 3 in Tromsø, while Oslo
 * needs a strong storm), with cloud over at most AURORA_MAX_CLOUD percent
 * of the sky, and dark (Yr's "sunlight" anything but "day"). */
#define AURORA_MIN_VALUE 0.3f
#define AURORA_MAX_CLOUD 75

typedef struct {
    int64_t start, end; /* epoch seconds */
    float value;        /* 0..1 */
    uint8_t cloud;      /* percent of the sky */
    uint8_t kp;
    bool dark;
} aurora_hour_t;

typedef struct {
    bool valid;
    int count;
    aurora_hour_t hours[AURORA_MAX_HOURS];
} aurora_t;

/* Fill `out` from Yr's JSON; false if it doesn't read as one. */
bool aurora_parse(const char *json, aurora_t *out);

/* Whether hour `h` is one to show (see AURORA_MIN_VALUE). */
static inline bool aurora_good(const aurora_hour_t *h)
{
    return h->dark && h->value >= AURORA_MIN_VALUE && h->cloud <= AURORA_MAX_CLOUD;
}

/* Fetch the forecast for (lat, lon), as http_get_body_cached does: may be
 * HTTP_NOT_MODIFIED with `out` untouched. */
esp_err_t aurora_client_fetch(double lat, double lon, aurora_t *out, http_cache_t *cache);

#endif
