#include <stdlib.h>
#include "check.h"
#include "../../main/sun.c"

/* Local "HH:MM" of `t`, in Norway's time zone. */
static const char *hhmm(time_t t)
{
    static char s[8];
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(s, sizeof(s), "%H:%M", &lt);
    return s;
}

static time_t noon(int y, int m, int d)
{
    struct tm tm = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = 12, .tm_isdst = -1 };
    return mktime(&tm);
}

void test_sun(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    time_t r, s;
    /* Published times (timeanddate.com), to the minute or one off. */
    CHECK(sun_times(59.91, 10.75, noon(2026, 9, 28), &r, &s) == SUN_RISES_AND_SETS);
    CHECK_STR(hhmm(r), "07:15");
    CHECK_STR(hhmm(s), "18:58");
    CHECK(sun_times(59.91, 10.75, noon(2026, 12, 21), &r, &s) == SUN_RISES_AND_SETS);
    CHECK_STR(hhmm(s), "15:12");
    CHECK(sun_times(69.68, 18.94, noon(2026, 12, 21), &r, &s) == SUN_DOWN_ALL_DAY);  /* Tromsø: mørketid */
    CHECK(sun_times(69.68, 18.94, noon(2026, 6, 21), &r, &s) == SUN_UP_ALL_DAY);     /* midnattssol */
    CHECK(sun_elevation_deg(59.91, 10.75, noon(2026, 6, 21)) > 50);
    CHECK(sun_elevation_deg(59.91, 10.75, noon(2026, 6, 21) + 12 * 3600) < 0);
}
