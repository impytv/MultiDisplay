/* Norwegian public holidays and week numbers (see holidays.h). The table is
 * generated from norskkalender.no by scripts/build_holidays.py. */

#include <stddef.h>

#include "holidays.h"
#include "holidays_data.h"

const char *holiday_name(int year, int month, int day)
{
    for (size_t i = 0; i < sizeof(HOLIDAYS) / sizeof(HOLIDAYS[0]); i++) {
        const holiday_t *h = &HOLIDAYS[i];
        if (h->year == year && h->month == month && h->day == day) {
            return h->name;
        }
    }
    return NULL;
}

bool holidays_known(int year)
{
    return year >= HOLIDAYS_FIRST_YEAR && year <= HOLIDAYS_LAST_YEAR;
}

int holidays_last_year(void)
{
    return HOLIDAYS_LAST_YEAR;
}

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil). */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

/* 0 = Monday ... 6 = Sunday (1970-01-01 was a Thursday). */
static int weekday_mon0(long days)
{
    return (int)(((days % 7) + 7 + 3) % 7);
}

int iso_week(int year, int month, int day)
{
    const long days = days_from_civil(year, month, day);
    const long thursday = days - weekday_mon0(days) + 3; /* the Thursday of its week decides the year */
    /* That Thursday's year: the date's own, or the one before or after. */
    int y = year;
    if (thursday < days_from_civil(y, 1, 1)) {
        y--;
    } else if (thursday >= days_from_civil(y + 1, 1, 1)) {
        y++;
    }
    return (int)((thursday - days_from_civil(y, 1, 1)) / 7) + 1;
}
