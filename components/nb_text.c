#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "civil_time.h"
#include "nb_text.h"

const char *const NB_WEEKDAY[7] = { "s\xC3\xB8ndag", "mandag",  "tirsdag", "onsdag",
                                    "torsdag",       "fredag", "l\xC3\xB8rdag" };
const char *const NB_WEEKDAY_SHORT[7] = { "s\xC3\xB8n", "man", "tir", "ons", "tor", "fre", "l\xC3\xB8r" };
const char *const NB_MONTH[12] = { "januar", "februar", "mars",      "april",   "mai",      "juni",
                                   "juli",   "august",  "september", "oktober", "november", "desember" };

time_t nb_midnight(time_t t, int days)
{
    struct tm lt;
    localtime_r(&t, &lt);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_mday += days;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

time_t nb_next_local(time_t now, int hour, int min)
{
    /* By the clock on the wall: past hour:min today means tomorrow's, even
     * in the hour that comes twice in October (where mktime may take
     * either 02:00). */
    struct tm lt;
    localtime_r(&now, &lt);
    const bool passed = lt.tm_hour * 60 + lt.tm_min >= hour * 60 + min;
    lt.tm_mday += passed ? 1 : 0;
    lt.tm_hour = hour;
    lt.tm_min = min;
    lt.tm_sec = 0;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

int64_t nb_day_number(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    return days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

void nb_day_word(char *out, size_t n, time_t t, time_t now)
{
    const int64_t days = nb_day_number(t) - nb_day_number(now);
    struct tm lt;
    localtime_r(&t, &lt);
    if (days == 0) {
        snprintf(out, n, "i dag");
    } else if (days == 1) {
        snprintf(out, n, "i morgen");
    } else if (days == -1) {
        snprintf(out, n, "i g\xC3\xA5r");
    } else if (days > 1 && days < 7) {
        snprintf(out, n, "%s", NB_WEEKDAY[lt.tm_wday]);
    } else {
        snprintf(out, n, "%d.%d.", lt.tm_mday, lt.tm_mon + 1);
    }
}

void nb_day_time(char *out, size_t n, time_t t, time_t now)
{
    char day[16];
    nb_day_word(day, sizeof(day), t, now);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(out, n, "%s %02d:%02d", day, lt.tm_hour, lt.tm_min);
}

void nb_updated(char *out, size_t n, time_t when, time_t now)
{
    struct tm lt;
    localtime_r(&when, &lt);
    if (nb_day_number(when) == nb_day_number(now)) {
        snprintf(out, n, "Sist oppdatert kl. %02d:%02d", lt.tm_hour, lt.tm_min);
        return;
    }
    char day[16];
    const int64_t ago = nb_day_number(now) - nb_day_number(when);
    if (ago == 1) {
        snprintf(day, sizeof(day), "i g\xC3\xA5r");
    } else if (ago > 1 && ago < 7) {
        snprintf(day, sizeof(day), "%s", NB_WEEKDAY[lt.tm_wday]);
    } else {
        snprintf(day, sizeof(day), "%d.%d.", lt.tm_mday, lt.tm_mon + 1);
    }
    snprintf(out, n, "Sist oppdatert %s kl. %02d:%02d", day, lt.tm_hour, lt.tm_min);
}

int nb_round(double v)
{
    const long r = lround(v);
    return r == 0 ? 0 : (int)r;
}

void nb_dec(char *out, size_t n, double v, int decimals)
{
    if (decimals <= 0) {
        snprintf(out, n, "%d", nb_round(v));
        return;
    }
    const double scale = pow(10.0, decimals);
    const double r = round(v * scale) / scale;
    snprintf(out, n, "%.*f", decimals, r == 0.0 ? 0.0 : r); /* -0.0 == 0.0 */
    char *dot = strchr(out, '.');
    if (dot != NULL) {
        *dot = ',';
    }
}

void nb_temp(char *out, size_t n, double c)
{
    snprintf(out, n, "%d\xC2\xB0", nb_round(c));
}

void nb_mm_range(char *out, size_t n, double lo, double hi)
{
    char a[16], b[16];
    nb_dec(a, sizeof(a), lo, 1);
    nb_dec(b, sizeof(b), hi, 1);
    if (strcmp(a, b) == 0) {
        snprintf(out, n, "%s mm", b);
    } else {
        snprintf(out, n, "%s\xE2\x80\x93%s mm", a, b);
    }
}

void nb_departure(char *out, size_t n, int64_t t, time_t now, bool short_min)
{
    const int64_t mins = (t - (int64_t)now) / 60;
    if (mins < 1) {
        snprintf(out, n, "N\xC3\xA5");
    } else if (mins < 15) {
        snprintf(out, n, short_min ? "%d m" : "%d min", (int)mins);
    } else {
        const time_t tt = (time_t)t;
        struct tm lt;
        localtime_r(&tt, &lt);
        snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
}

bool nb_rain_change(char *out, size_t n, bool starts, int64_t left_s)
{
    if (left_s <= 0) {
        return false;
    }
    const int min = (int)((left_s + 299) / 300 * 5);
    snprintf(out, n, starts ? "Nedb\xC3\xB8r om %d min" : "Opphold om %d min", min);
    return true;
}
