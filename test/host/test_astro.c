/* The astronomical events on the month calendar, against Skyfield's own
 * figures (scripts/build_astro.py prints them for Oslo). */

#include <math.h>
#include <stdlib.h>

#include "astro.h"
#include "check.h"

#define OSLO_LAT 59.9139
#define OSLO_LON 10.7522

static time_t utc(int y, int m, int d)
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d };
    return timegm(&t);
}

/* The events on a UTC day (the tests run in UTC). */
static int day(int y, int m, int d, double lat, double lon, astro_item_t *out)
{
    return astro_day(utc(y, m, d), utc(y, m, d) + 86400, lat, lon, out, 8);
}

static const astro_item_t *find(const astro_item_t *it, int n, astro_kind_t kind)
{
    for (int i = 0; i < n; i++) {
        if (it[i].kind == kind) {
            return &it[i];
        }
    }
    return NULL;
}

static bool has(const astro_item_t *it, int n, astro_kind_t kind)
{
    return find(it, n, kind) != NULL;
}

void test_astro(void)
{
    astro_item_t it[8];
    int n;

    /* The partial solar eclipse of 12 August 2026: Skyfield says 83.0 %
     * covered in Oslo, at about 17:4x UTC. */
    time_t when = 0;
    bool annular = true;
    const double c = astro_solar_cover(1, OSLO_LAT, OSLO_LON, &when, &annular);
    CHECK(fabs(c - 0.830) < 0.01);
    CHECK(!annular);
    CHECK(when >= utc(2026, 8, 12) + 17 * 3600 + 30 * 60 && when <= utc(2026, 8, 12) + 18 * 3600);
    n = day(2026, 8, 12, OSLO_LAT, OSLO_LON, it);
    /* In time order: new moon 17:36, greatest in Oslo 17:56, the Perseids' night. */
    CHECK(n == 3 && it[1].kind == ASTRO_SOLAR_ECLIPSE && abs(it[1].value - 83) <= 1);
    CHECK(it[0].kind == ASTRO_NEW_MOON && it[2].kind == ASTRO_METEORS);
    /* 2 August 2027 (13.9 %) and 1 June 2030 (44.5 %, annular elsewhere). */
    n = day(2027, 8, 2, OSLO_LAT, OSLO_LON, it);
    CHECK(has(it, n, ASTRO_SOLAR_ECLIPSE) && abs(find(it, n, ASTRO_SOLAR_ECLIPSE)->value - 14) <= 1);
    n = day(2030, 6, 1, OSLO_LAT, OSLO_LON, it);
    CHECK(has(it, n, ASTRO_SOLAR_ECLIPSE));
    /* 17 February 2026's is in Antarctica: nothing in Oslo. */
    n = day(2026, 2, 17, OSLO_LAT, OSLO_LON, it);
    CHECK(!has(it, n, ASTRO_SOLAR_ECLIPSE) && has(it, n, ASTRO_NEW_MOON));

    /* The total lunar eclipse of 3 March 2026 (11:33 UTC) is in daylight in
     * Norway; the partial one of 28 August 2026 (04:13 UTC) is at moonset,
     * but seen for the hours before. */
    n = day(2026, 3, 3, OSLO_LAT, OSLO_LON, it);
    CHECK(!has(it, n, ASTRO_LUNAR_ECLIPSE) && has(it, n, ASTRO_FULL_MOON));
    n = day(2026, 8, 28, OSLO_LAT, OSLO_LON, it);
    CHECK(has(it, n, ASTRO_LUNAR_ECLIPSE) && find(it, n, ASTRO_LUNAR_ECLIPSE)->value == 1);
    /* Seen from Los Angeles, the March one is (03:33 local). */
    n = day(2026, 3, 3, 34.05, -118.24, it);
    CHECK(has(it, n, ASTRO_LUNAR_ECLIPSE));

    /* Seasons, phases, showers. */
    n = day(2026, 6, 21, OSLO_LAT, OSLO_LON, it);
    CHECK(n == 2 && it[0].kind == ASTRO_JUNE_SOLSTICE && it[0].t == 1782030270); /* 08:24 UTC */
    CHECK(it[1].kind == ASTRO_FIRST_QUARTER);
    n = day(2026, 12, 24, OSLO_LAT, OSLO_LON, it);
    CHECK(has(it, n, ASTRO_FULL_MOON) && has(it, n, ASTRO_SUPERMOON));
    n = day(2026, 8, 12, OSLO_LAT, OSLO_LON, it); /* the Perseids peak 13 Aug 02:08 UTC: the night from the 12th */
    CHECK(has(it, n, ASTRO_METEORS));
    CHECK_STR(astro_shower_name(2), "Perseidene");
    n = day(2026, 8, 13, OSLO_LAT, OSLO_LON, it);
    CHECK(!has(it, n, ASTRO_METEORS));
    n = day(2026, 10, 14, OSLO_LAT, OSLO_LON, it);
    CHECK(n == 0);
    CHECK(astro_known(2026) && astro_known(2035) && !astro_known(2036));
}
