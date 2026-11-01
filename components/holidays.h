#pragma once

/* Norwegian public holidays ("røde dager") and ISO week numbers, for the
 * month calendar (main/month.c). */

#include <stdbool.h>
#include <stdint.h>

/* The holiday's name on that date ("1. juledag"; two on one day joined by
 * " / "), or NULL if it isn't one or the year is outside holidays_known.
 * Sundays that are no holiday aren't. Computed from Easter. */
const char *holiday_name(int year, int month, int day);

/* The years they are given for, by today's rules. */
#define HOLIDAYS_FIRST 1900
#define HOLIDAYS_LAST  2199
bool holidays_known(int year);


/* The ISO 8601 week number (1-53; Monday first, week 1 holds the year's
 * first Thursday), as used in Norway. */
int iso_week(int year, int month, int day);
