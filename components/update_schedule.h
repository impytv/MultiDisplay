#ifndef _UPDATE_SCHEDULE_H_
#define _UPDATE_SCHEDULE_H_

#include <stdint.h>
#include <time.h>

#include "civil_time.h"

/* The firmware update checks (main/updater.c) fall at a local time of day,
 * `at` (minutes after midnight), and every `every_h` hours from there: with
 * 24, once a day at `at`; with 6 and 03:30, at 03:30, 09:30, 15:30 and
 * 21:30. The periods between them are numbered, so a check is due when the
 * number changes. */

/* Local time as minutes since 1970-01-01 00:00 local. */
static inline int64_t upd_local_min(const struct tm *lt)
{
    return days_from_civil(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday) * 1440 + lt->tm_hour * 60 +
           lt->tm_min;
}

/* The number of the period `local_min` is in; *into is how many minutes
 * into it. */
static inline int64_t upd_period(int64_t local_min, int at, int every_h, int *into)
{
    const int64_t len = (int64_t)every_h * 60;
    const int64_t t = local_min - at;
    int64_t n = t / len;
    if (t % len < 0) {
        n--;
    }
    *into = (int)(t - n * len);
    return n;
}

/* The local time of day (minutes after midnight) period `n` starts. */
static inline int upd_period_start_min(int64_t n, int at, int every_h)
{
    int64_t m = (n * every_h * 60 + at) % 1440;
    return (int)(m < 0 ? m + 1440 : m);
}

#endif
