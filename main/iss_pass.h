#pragma once

/* When the ISS next passes over a place, and whether it can be seen then
 * (see iss_pass.c). Pure C apart from the sun (sun.c), so it is
 * host-tested. */

#include <stdbool.h>

#include "sgp4.h"

/* A pass counts from when the station is this high over the horizon: lower
 * than this, houses and trees hide it. */
#define ISS_PASS_MIN_EL 10.0

typedef struct {
    double rise, top, set; /* epoch seconds: above ISS_PASS_MIN_EL, highest, below again */
    double max_el;         /* degrees, at top */
    /* The part of it that can be seen (the sky dark there, the station in
     * sunlight), seen_from = 0 if none, and how high it gets meanwhile. */
    double seen_from, seen_to, seen_max_el;
    bool dark_at_top; /* the sun 6 degrees or more below the horizon at top */
} iss_pass_t;

/* The first pass over lat/lon (degrees, east positive) that hasn't ended
 * at `from` and begins before `until` - one already under way included -
 * or, if seen_only, the first with a part that can be seen. False if
 * there's none in that time (or the elements give out). */
bool iss_next_pass(const sgp4_t *s, double lat, double lon, double from, double until, bool seen_only,
                   iss_pass_t *p);
