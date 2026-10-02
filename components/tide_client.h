#ifndef _TIDE_CLIENT_H_
#define _TIDE_CLIENT_H_

/* Tides and water level from Kartverket's tide API (vannstand.kartverket.no,
 * "Se havnivå"): for a point on the Norwegian coast, the nearest permanent
 * station's
 *
 *  - prediction: the astronomical tide;
 *  - forecast: the tide plus the weather's effect (wind, air pressure), a
 *    few days ahead;
 *  - observation: what the station measured, up to now;
 *  - the times and heights of high and low tide.
 *
 * In centimetres above chart datum (sjøkartnull). Inland points get no data
 * ("too far from the coast"). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TIDE_STEP_S    1200 /* one value every 20 minutes */
#define TIDE_POINTS    145  /* 48 hours */
#define TIDE_EXTREMES  12
#define TIDE_NAME_MAX  40

typedef struct {
    int64_t time;
    float cm;
    bool high; /* high tide, else low */
} tide_extreme_t;

typedef struct {
    bool valid;     /* parsed; may still be without data */
    bool no_data;   /* the point is too far from the coast */
    char station[TIDE_NAME_MAX];
    int64_t start;  /* epoch seconds of point 0; TIDE_STEP_S apart */
    int count;
    float prediction[TIDE_POINTS];  /* NAN where not given */
    float forecast[TIDE_POINTS];
    float observation[TIDE_POINTS];
    int extreme_count;
    tide_extreme_t extremes[TIDE_EXTREMES];
} tide_t;

/* Fetch the 48 hours from 6 hours before `now` (epoch seconds) for the
 * point lat, lon. */
esp_err_t tide_fetch(double lat, double lon, int64_t now, tide_t *out);

/* The parsers, for the host tests: the series reply (datatype=ALL) into a
 * tide_t whose start and count are set, and the high/low reply
 * (datatype=TAB) into its extremes. Both say whether the reply was one of
 * Kartverket's; a "no data here" reply counts and sets no_data. */
bool tide_parse_series(const char *xml, tide_t *out);
bool tide_parse_extremes(const char *xml, tide_t *out);

/* The value of `series` at time t, interpolated, or NAN outside it or in a
 * gap. */
float tide_at(const tide_t *t, const float *series, int64_t when);

#endif
