#include <stdlib.h>

#include "check.h"
#include "civil_time.h"
#include "nb_text.h"

/* y-m-d h:mi in UTC as epoch seconds. */
static time_t utc(int y, int m, int d, int h, int mi, int s)
{
    return (time_t)(days_from_civil(y, m, d) * 86400 + h * 3600 + mi * 60 + s);
}

void test_nb_text(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); /* as app_main */
    tzset();
    char buf[64];

    /* Midnights across the October change: 25 hours that day. */
    const time_t oct25_noon = utc(2026, 10, 25, 11, 0, 0);
    CHECK_INT(nb_midnight(oct25_noon, 0), utc(2026, 10, 24, 22, 0, 0));
    CHECK_INT(nb_midnight(oct25_noon, 1), utc(2026, 10, 25, 23, 0, 0));
    CHECK_INT(nb_midnight(oct25_noon, -1), utc(2026, 10, 23, 22, 0, 0));
    CHECK_INT(nb_midnight(utc(2026, 3, 29, 12, 0, 0), 1), utc(2026, 3, 29, 22, 0, 0));

    /* The nightly restart: one a night, also on the change nights. */
    CHECK_INT(nb_next_local(utc(2026, 11, 4, 21, 0, 0), 2, 0), utc(2026, 11, 5, 1, 0, 0));
    CHECK_INT(nb_next_local(utc(2026, 11, 5, 1, 0, 5), 2, 0), utc(2026, 11, 6, 1, 0, 0));
    /* Booted after the first 02:00 (CEST) and after the second (CET):
     * Monday's 02:00 CET both times, nothing in between. */
    CHECK_INT(nb_next_local(utc(2026, 10, 25, 0, 0, 15), 2, 0), utc(2026, 10, 26, 1, 0, 0));
    CHECK_INT(nb_next_local(utc(2026, 10, 25, 1, 0, 15), 2, 0), utc(2026, 10, 26, 1, 0, 0));
    /* Before it that night: one of the two 02:00s. */
    const time_t t = nb_next_local(utc(2026, 10, 24, 22, 30, 0), 2, 0);
    CHECK(t == utc(2026, 10, 25, 0, 0, 0) || t == utc(2026, 10, 25, 1, 0, 0));
    /* March: no 02:00 that night, so 03:00 CEST, then 02:00 again. */
    CHECK_INT(nb_next_local(utc(2026, 3, 29, 0, 30, 0), 2, 0), utc(2026, 3, 29, 1, 0, 0));
    CHECK_INT(nb_next_local(utc(2026, 3, 29, 1, 0, 10), 2, 0), utc(2026, 3, 30, 0, 0, 0));
    CHECK_INT(nb_next_local(utc(2026, 3, 28, 1, 0, 10), 2, 0), utc(2026, 3, 29, 1, 0, 0));

    /* Days by the local date, also over the year's end. */
    const time_t dec31 = utc(2026, 12, 31, 22, 30, 0); /* 23:30 local */
    CHECK_INT(nb_day_number(dec31) + 1, nb_day_number(utc(2026, 12, 31, 23, 0, 0)));
    nb_day_word(buf, sizeof(buf), utc(2026, 12, 31, 23, 30, 0), dec31);
    CHECK_STR(buf, "i morgen");
    nb_day_word(buf, sizeof(buf), utc(2026, 12, 31, 10, 0, 0), dec31);
    CHECK_STR(buf, "i dag");
    nb_day_word(buf, sizeof(buf), utc(2026, 12, 30, 10, 0, 0), dec31);
    CHECK_STR(buf, "i g\xC3\xA5r");
    nb_day_word(buf, sizeof(buf), utc(2027, 1, 2, 10, 0, 0), dec31);
    CHECK_STR(buf, "l\xC3\xB8rdag");
    nb_day_word(buf, sizeof(buf), utc(2027, 1, 7, 10, 0, 0), dec31);
    CHECK_STR(buf, "7.1.");
    nb_day_time(buf, sizeof(buf), utc(2026, 12, 31, 23, 5, 0), dec31);
    CHECK_STR(buf, "i morgen 00:05");
    nb_day_time(buf, sizeof(buf), utc(2026, 11, 4, 18, 42, 0), utc(2026, 11, 4, 12, 0, 0));
    CHECK_STR(buf, "i dag 19:42");

    /* How old the data is. */
    const time_t mon = utc(2026, 11, 9, 7, 0, 0);
    nb_updated(buf, sizeof(buf), utc(2026, 11, 9, 5, 2, 0), mon);
    CHECK_STR(buf, "Sist oppdatert kl. 06:02");
    nb_updated(buf, sizeof(buf), utc(2026, 11, 8, 13, 2, 0), mon);
    CHECK_STR(buf, "Sist oppdatert i g\xC3\xA5r kl. 14:02");
    nb_updated(buf, sizeof(buf), utc(2026, 11, 6, 13, 2, 0), mon);
    CHECK_STR(buf, "Sist oppdatert fredag kl. 14:02");
    nb_updated(buf, sizeof(buf), utc(2026, 10, 30, 13, 2, 0), mon);
    CHECK_STR(buf, "Sist oppdatert 30.10. kl. 14:02");

    /* Numbers: a comma, halves away from zero, never -0. */
    CHECK_INT(nb_round(2.5), 3);
    CHECK_INT(nb_round(-2.5), -3);
    CHECK_INT(nb_round(-0.3), 0);
    CHECK_INT(nb_round(-0.5), -1);
    nb_dec(buf, sizeof(buf), 3.45, 1);
    CHECK_STR(buf, "3,5");
    nb_dec(buf, sizeof(buf), -0.04, 1);
    CHECK_STR(buf, "0,0");
    nb_dec(buf, sizeof(buf), -0.3, 1);
    CHECK_STR(buf, "-0,3");
    nb_dec(buf, sizeof(buf), -0.3, 0);
    CHECK_STR(buf, "0");
    nb_dec(buf, sizeof(buf), 10.66, 1);
    CHECK_STR(buf, "10,7");
    nb_dec(buf, sizeof(buf), 12, 0);
    CHECK_STR(buf, "12");
    nb_temp(buf, sizeof(buf), -0.3);
    CHECK_STR(buf, "0\xC2\xB0");
    nb_temp(buf, sizeof(buf), -3.6);
    CHECK_STR(buf, "-4\xC2\xB0");
    nb_mm_range(buf, sizeof(buf), 0.3, 1.2);
    CHECK_STR(buf, "0,3\xE2\x80\x93" "1,2 mm");
    nb_mm_range(buf, sizeof(buf), 1.2, 1.2);
    CHECK_STR(buf, "1,2 mm");
    nb_mm_range(buf, sizeof(buf), 1.21, 1.24);
    CHECK_STR(buf, "1,2 mm");
    nb_mm_range(buf, sizeof(buf), 0.0, 2.3);
    CHECK_STR(buf, "0,0\xE2\x80\x93" "2,3 mm");

    /* Departures: "Nå" from under a minute, also once due. */
    const time_t at = utc(2026, 11, 5, 7, 0, 0); /* 08:00 local */
    nb_departure(buf, sizeof(buf), at + 59, at, false);
    CHECK_STR(buf, "N\xC3\xA5");
    nb_departure(buf, sizeof(buf), at - 30, at, false);
    CHECK_STR(buf, "N\xC3\xA5");
    nb_departure(buf, sizeof(buf), at + 60, at, false);
    CHECK_STR(buf, "1 min");
    nb_departure(buf, sizeof(buf), at + 14 * 60 + 59, at, true);
    CHECK_STR(buf, "14 m");
    nb_departure(buf, sizeof(buf), at + 15 * 60, at, false);
    CHECK_STR(buf, "08:15");
    nb_departure(buf, sizeof(buf), utc(2026, 11, 5, 23, 5, 0), utc(2026, 11, 5, 22, 0, 0), false);
    CHECK_STR(buf, "00:05"); /* past midnight */

    /* The nowcast's note: five minutes at a time, rounded up. */
    CHECK(nb_rain_change(buf, sizeof(buf), true, 1));
    CHECK_STR(buf, "Nedb\xC3\xB8r om 5 min");
    CHECK(nb_rain_change(buf, sizeof(buf), false, 301));
    CHECK_STR(buf, "Opphold om 10 min");
    CHECK(nb_rain_change(buf, sizeof(buf), true, 900));
    CHECK_STR(buf, "Nedb\xC3\xB8r om 15 min");
    CHECK(!nb_rain_change(buf, sizeof(buf), true, 0));
    CHECK(!nb_rain_change(buf, sizeof(buf), false, -60));
}
