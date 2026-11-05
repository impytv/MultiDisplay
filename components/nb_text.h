#ifndef _NB_TEXT_H_
#define _NB_TEXT_H_

/* Dates, times and numbers as the screens write them: Norwegian (bokmål),
 * the 24-hour clock and a decimal comma, one way on every screen. Local
 * time is the TZ set in app_main. Pure C, host tested. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Names by tm_wday (Sunday = 0) and tm_mon, in UTF-8: "søndag", "søn",
 * "januar". */
extern const char *const NB_WEEKDAY[7];
extern const char *const NB_WEEKDAY_SHORT[7];
extern const char *const NB_MONTH[12];

/* Local midnight starting the day `days` after `t`'s (0 = its own). Moves
 * by calendar days, so it is right across a DST change, unlike adding
 * 24 h. */
time_t nb_midnight(time_t t, int days);

/* The next local `hour`:`min`: today's if the clock hasn't reached it yet,
 * otherwise the next day's (where a missing hour in March makes it an hour
 * later). */
time_t nb_next_local(time_t now, int hour, int min);

/* `t`'s local date as days since 1970-01-01, for telling days apart and
 * counting them. */
int64_t nb_day_number(time_t t);

/* `t`'s day from `now`'s: "i dag", "i morgen", "i går", the weekday
 * ("fredag") up to 6 days ahead, otherwise the date ("5.11."). */
void nb_day_word(char *out, size_t n, time_t t, time_t now);

/* The same with the time: "i dag 19:42", "i morgen 05:51". */
void nb_day_time(char *out, size_t n, time_t t, time_t now);

/* "Sist oppdatert kl. 14:02" for data from today, and with the day when it
 * is older: "Sist oppdatert i går kl. 14:02", "... fredag kl. 14:02". */
void nb_updated(char *out, size_t n, time_t when, time_t now);

/* `v` rounded to a whole number, halves away from zero, never -0. */
int nb_round(double v);

/* `v` with `decimals` decimals and a decimal comma: "3,4", "-0,3", "12";
 * never "-0" or "-0,0". */
void nb_dec(char *out, size_t n, double v, int decimals);

/* A temperature: "-3°", "0°", "12°" - rounded as nb_round. */
void nb_temp(char *out, size_t n, double c);

/* Precipitation from `lo` to `hi` mm at a tenth: "0,3–1,2 mm", or one
 * amount when they are the same at a tenth ("1,2 mm"). */
void nb_mm_range(char *out, size_t n, double lo, double hi);

/* A departure at `t`: "Nå" under a minute (and once it is due), "N min"
 * under 15 ("N m" if `short_min`), otherwise "HH:MM" - as Entur's pages. */
void nb_departure(char *out, size_t n, int64_t t, time_t now, bool short_min);

/* The nowcast's note, `left_s` seconds before the change it tells of:
 * "Nedbør om 15 min" (`starts`), "Opphold om 5 min" (`stops`), whole five
 * minutes rounded up; false once that time has come (the next nowcast says
 * more). */
bool nb_rain_change(char *out, size_t n, bool starts, int64_t left_s);

#endif
