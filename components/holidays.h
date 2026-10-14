#pragma once

/* Norwegian public holidays ("røde dager") and ISO week numbers, for the
 * month calendar (main/month.c). */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t year;
    uint8_t month; /* 1-12 */
    uint8_t day;
    const char *name;
} holiday_t;

/* The holiday's name on that date ("1. juledag"), or NULL if it isn't one
 * or the year is outside the table (see holidays_known). Sundays that are
 * no holiday aren't in it. */
const char *holiday_name(int year, int month, int day);

/* Whether the table has the holidays for `year` (scripts/build_holidays.py
 * reads them from norskkalender.no, a few years ahead). */
bool holidays_known(int year);

/* The last year in the table. */
int holidays_last_year(void);

/* The ISO 8601 week number (1-53; Monday first, week 1 holds the year's
 * first Thursday), as used in Norway. */
int iso_week(int year, int month, int day);
