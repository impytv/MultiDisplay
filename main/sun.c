/* The sun's position from the low-precision formulas of the Astronomical
 * Almanac (as used by NOAA and the US Naval Observatory): good to about
 * 0.01 degrees for decades around 2000. */

#include <math.h>

#include "sun.h"

#define RAD (M_PI / 180.0)
#define J2000_EPOCH 946728000.0 /* 2000-01-01 12:00 UTC */

double sun_elevation_deg(double lat, double lon, time_t t)
{
    const double d = ((double)t - J2000_EPOCH) / 86400.0;
    const double g = (357.529 + 0.98560028 * d) * RAD;   /* mean anomaly */
    const double q = 280.459 + 0.98564736 * d;           /* mean longitude */
    const double l = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * RAD; /* ecliptic longitude */
    const double e = (23.439 - 0.00000036 * d) * RAD;    /* obliquity */
    const double ra = atan2(cos(e) * sin(l), cos(l));
    const double dec = asin(sin(e) * sin(l));
    const double gmst_deg = fmod(280.46061837 + 360.98564736629 * d, 360.0);
    const double ha = (gmst_deg + lon) * RAD - ra;       /* hour angle */
    const double phi = lat * RAD;
    return asin(sin(phi) * sin(dec) + cos(phi) * cos(dec) * cos(ha)) / RAD;
}

/* Between a and b (a on one side of the horizon, b on the other), to 30 s. */
static time_t crossing(double lat, double lon, time_t a, time_t b)
{
    const bool a_up = sun_elevation_deg(lat, lon, a) > SUN_DOWN_DEG;
    while (b - a > 30) {
        const time_t m = a + (b - a) / 2;
        if ((sun_elevation_deg(lat, lon, m) > SUN_DOWN_DEG) == a_up) {
            a = m;
        } else {
            b = m;
        }
    }
    return a + (b - a) / 2;
}

sun_day_t sun_times(double lat, double lon, time_t day, time_t *rise, time_t *set)
{
    struct tm lt;
    localtime_r(&day, &lt);
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    lt.tm_isdst = -1;
    const time_t start = mktime(&lt);
    *rise = *set = 0;

    /* Every 20 minutes through the day, and between two samples on either
     * side of the horizon, find the moment. */
    bool up = sun_elevation_deg(lat, lon, start) > SUN_DOWN_DEG;
    const bool up_at_start = up;
    bool crossed = false;
    for (time_t t = start + 1200; t <= start + 24 * 3600; t += 1200) {
        const bool now_up = sun_elevation_deg(lat, lon, t) > SUN_DOWN_DEG;
        if (now_up != up) {
            const time_t c = crossing(lat, lon, t - 1200, t);
            if (now_up && *rise == 0) {
                *rise = c;
            } else if (!now_up && *set == 0) {
                *set = c;
            }
            crossed = true;
            up = now_up;
        }
    }
    if (!crossed) {
        return up_at_start ? SUN_UP_ALL_DAY : SUN_DOWN_ALL_DAY;
    }
    return SUN_RISES_AND_SETS;
}
