#pragma once

/* Astronomical events for the month calendar (main/month.c): the moon's
 * phases, supermoons, equinoxes and solstices, meteor shower peaks and the
 * eclipses that can be seen from a given place. The events come from a
 * table built by scripts/build_astro.py; how much of the sun an eclipse
 * covers where the display is, is worked out here. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    ASTRO_NEW_MOON,
    ASTRO_FIRST_QUARTER,
    ASTRO_FULL_MOON,
    ASTRO_LAST_QUARTER,
    ASTRO_SUPERMOON,      /* a full moon closer than 360 000 km */
    ASTRO_MARCH_EQUINOX,
    ASTRO_JUNE_SOLSTICE,
    ASTRO_SEPTEMBER_EQUINOX,
    ASTRO_DECEMBER_SOLSTICE,
    ASTRO_METEORS,        /* arg: which shower (astro_shower_name) */
    ASTRO_LUNAR_ECLIPSE,  /* arg: 1 partial, 2 total */
    ASTRO_SOLAR_ECLIPSE,  /* arg: its track in the table */
} astro_kind_t;

typedef struct {
    uint32_t t; /* UTC */
    uint8_t kind;
    uint8_t arg;
} astro_event_t;

/* One event on a day, as found for a place. */
typedef struct {
    astro_kind_t kind;
    time_t t;    /* when (for a solar eclipse, its greatest there) */
    int value;   /* lunar eclipse: 1 partial, 2 total; solar: per cent of the
                    sun covered (100 total; see `annular`); meteors: which */
    bool annular;
} astro_item_t;

/* The events of the day from day_start up to day_end (local midnights),
 * seen from lat/lon (degrees, east positive): eclipses only if they can be
 * seen there (the sun up for a solar one, down for a lunar one), meteor
 * showers on the evening the night of their peak begins. Up to `max`, in
 * time order; returns how many. */
int astro_day(time_t day_start, time_t day_end, double lat, double lon, astro_item_t *out, int max);

/* The shower's name ("Perseidene"). */
const char *astro_shower_name(int which);

/* The share (0..1) of the sun covered at its greatest during solar eclipse
 * `track` at lat/lon with the sun up, and when; 0 if not seen there. */
double astro_solar_cover(int track, double lat, double lon, time_t *when, bool *annular);

/* Whether the table covers `year`, and the last year it does. */
bool astro_known(int year);
int astro_last_year(void);
