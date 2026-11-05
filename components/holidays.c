/* Norwegian public holidays and week numbers (see holidays.h). The
 * holidays are computed: five on fixed dates, eight from Easter. */

#include <stddef.h>

#include "civil_time.h"
#include "holidays.h"

/* 0 = Monday ... 6 = Sunday (1970-01-01 was a Thursday). */
static int weekday_mon0(long days)
{
    return (int)(((days % 7) + 7 + 3) % 7);
}


/* Easter Sunday in the Gregorian calendar (the anonymous algorithm in
 * Meeus, "Astronomical Algorithms"): month and day. */
static void easter(int y, int *month, int *day)
{
    const int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4;
    const int f = (b + 8) / 25, g = (b - f + 1) / 3, h = (19 * a + b - d - g + 15) % 30;
    const int i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
    const int m = (a + 11 * h + 22 * l) / 451;
    *month = (h + l - 7 * m + 114) / 31;
    *day = (h + l - 7 * m + 114) % 31 + 1;
}

static const struct {
    int offset; /* days from Easter Sunday */
    const char *name;
} FROM_EASTER[] = {
    { -7, "Palmes\xC3\xB8" "ndag" },
    { -3, "Skj\xC3\xA6" "rtorsdag" },
    { -2, "Langfredag" },
    { 0, "1. p\xC3\xA5" "skedag" },
    { 1, "2. p\xC3\xA5" "skedag" },
    { 39, "Kristi Himmelfartsdag" },
    { 49, "1. pinsedag" },
    { 50, "2. pinsedag" },
};

const char *holiday_name(int year, int month, int day)
{
    if (!holidays_known(year)) {
        return NULL;
    }
    const char *fixed = NULL;
    if (month == 1 && day == 1) {
        fixed = "1. nytt\xC3\xA5" "rsdag";
    } else if (month == 5 && day == 1) {
        fixed = "Offentlig h\xC3\xB8" "ytidsdag";
    } else if (month == 5 && day == 17) {
        fixed = "Grunnlovsdag";
    } else if (month == 12 && day == 25) {
        fixed = "1. juledag";
    } else if (month == 12 && day == 26) {
        fixed = "2. juledag";
    }
    int em, ed;
    easter(year, &em, &ed);
    const long from_easter = days_from_civil(year, month, day) - days_from_civil(year, em, ed);
    const char *moving = NULL;
    for (size_t i = 0; i < sizeof(FROM_EASTER) / sizeof(FROM_EASTER[0]); i++) {
        if (FROM_EASTER[i].offset == from_easter) {
            moving = FROM_EASTER[i].name;
        }
    }
    if (fixed == NULL || moving == NULL) {
        return fixed != NULL ? fixed : moving;
    }
    /* On the same day: 1 May or 17 May with Ascension or Whitsun. */
    if (day == 1) {
        return "Offentlig h\xC3\xB8" "ytidsdag / Kristi Himmelfartsdag";
    }
    return from_easter == 39 ? "Grunnlovsdag / Kristi Himmelfartsdag"
         : from_easter == 49 ? "Grunnlovsdag / 1. pinsedag"
                             : "Grunnlovsdag / 2. pinsedag";
}

bool holidays_known(int year)
{
    return year >= HOLIDAYS_FIRST && year <= HOLIDAYS_LAST;
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
