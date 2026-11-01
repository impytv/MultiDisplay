/* The display's own events (own_events.h): when each is on, its text, and
 * keeping them in NVS. */

#include "own_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "own_events";

#define OWN_NVS_NS  "hendelser"
#define OWN_NVS_KEY "v2" /* the layout of own_event_t: a new key if it changes */
#define OWN_NVS_V1  "v1" /* before repeats: own_event_t without repeat and every */

typedef struct {
    char text[OWN_TEXT_MAX];
    uint16_t year;
    uint8_t month, day, kind, show;
} own_event_v1_t;

/* In the backup. */
static const char *const KIND_KEY[OWN_KINDS] = { "birthday", "anniversary", "appointment", "other" };
static const char *const SHOW_KEY[3] = { "calendar", "month", "clock" };
static const char *const REPEAT_KEY[OWN_REPEATS] = { "", "weekly", "monthly" };

static const char *const KIND_NAME[OWN_KINDS] = { "Bursdag", "Merkedag", "Avtale", "Annet" };
/* Apart from the calendars' blue, orange and green (calendar.c). */
static const uint32_t KIND_COLOUR[OWN_KINDS] = { 0xE84393, 0x9B59B6, 0x00A3A3, 0xC9A227 };

const char *own_kind_name(int kind)
{
    return kind >= 0 && kind < OWN_KINDS ? KIND_NAME[kind] : "";
}

uint32_t own_kind_colour(int kind)
{
    return kind >= 0 && kind < OWN_KINDS ? KIND_COLOUR[kind] : 0x888888;
}

static bool yearly(const own_event_t *e)
{
    return e->kind == OWN_BIRTHDAY || e->kind == OWN_ANNIVERSARY;
}

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int month_days(int y, int m)
{
    static const int D[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && (y == 0 || leap(y)) ? 29 : D[m - 1]; /* y 0: any year */
}

bool own_valid(const own_event_t *e)
{
    if (e->text[0] == '\0' || memchr(e->text, '\0', sizeof(e->text)) == NULL || e->kind >= OWN_KINDS ||
        (e->show & OWN_ON_ALL) == 0 || (e->show & ~OWN_ON_ALL) != 0) {
        return false;
    }
    if (e->year != 0 && (e->year < 1900 || e->year > 2200)) {
        return false;
    }
    if (e->year == 0 && !yearly(e)) {
        return false;
    }
    if (e->repeat >= OWN_REPEATS || (e->repeat != OWN_ONCE && yearly(e))) {
        return false;
    }
    if (e->repeat == OWN_ONCE ? e->every != 0
                              : e->every < 1 || e->every > (e->repeat == OWN_WEEKLY ? OWN_EVERY_WEEKS : OWN_EVERY_MONTHS)) {
        return false;
    }
    return e->month >= 1 && e->month <= 12 && e->day >= 1 && e->day <= month_days(e->year, e->month);
}

void own_title(const own_event_t *e, int year, char *dst, size_t len)
{
    const int n = e->year != 0 ? year - e->year : -1;
    if (e->kind == OWN_BIRTHDAY) {
        if (n > 0) {
            snprintf(dst, len, "%s fyller %d", e->text, n);
        } else {
            snprintf(dst, len, "%s har bursdag", e->text);
        }
    } else if (e->kind == OWN_ANNIVERSARY && n > 0) {
        snprintf(dst, len, "%s (%d \xC3\xA5r)", e->text, n);
    } else {
        snprintf(dst, len, "%s", e->text);
    }
}

/* Days since 1970-01-01 of a Gregorian date, and back (Howard Hinnant's
 * days_from_civil and civil_from_days). */
static long civil_days(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from(long z, int *y, int *m, int *d)
{
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

/* Local midnight starting day z (civil_days), and the midnight after. */
static int64_t midnight(long z, int64_t *end)
{
    int y, m, d;
    civil_from(z, &y, &m, &d);
    struct tm t = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_isdst = -1 };
    const int64_t start = (int64_t)mktime(&t);
    struct tm u = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d + 1, .tm_isdst = -1 };
    *end = (int64_t)mktime(&u);
    return start;
}

/* The local day (civil_days) t falls on. */
static long day_of(int64_t t)
{
    const time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    return civil_days(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

/* e's day in `year` (29 February on the 28th in other years). */
static long day_in(const own_event_t *e, int year)
{
    const int d = e->month == 2 && e->day == 29 && !leap(year) ? 28 : e->day;
    return civil_days(year, e->month, d);
}

/* A monthly event's day in month m of y: its day, or the month's last. */
static long day_in_month(const own_event_t *e, int y, int m)
{
    const int last = month_days(y, m);
    return civil_days(y, m, e->day < last ? e->day : last);
}

/* The first day e is on from day z on (z included), or -1 if none. */
static long first_from(const own_event_t *e, long z)
{
    if (yearly(e)) {
        int y, m, d;
        civil_from(z, &y, &m, &d);
        if (e->year > y) {
            y = e->year;
        }
        long day = day_in(e, y);
        return day >= z ? day : day_in(e, y + 1);
    }
    const long start = civil_days(e->year, e->month, e->day);
    if (z <= start || e->repeat == OWN_ONCE) {
        return z <= start ? start : -1;
    }
    const int n = e->every;
    if (e->repeat == OWN_WEEKLY) {
        const long step = 7L * n;
        return start + (z - start + step - 1) / step * step;
    }
    /* The first month of the series, counted from its own, not before z's;
     * then a step on if its day there is before z. */
    int y, m, d;
    civil_from(z, &y, &m, &d);
    const int since = (y - e->year) * 12 + (m - e->month); /* > 0 or 0: z is past the start */
    int k = (since + n - 1) / n * n;
    for (;; k += n) {
        const int months = (e->month - 1) + k;
        const long day = day_in_month(e, e->year + months / 12, months % 12 + 1);
        if (day >= z) {
            return day;
        }
    }
}

int own_days(const own_event_t *e, int n, int64_t from, int64_t to, uint8_t where, cal_list_t *out)
{
    if (to <= from) {
        return 0;
    }
    const long z0 = day_of(from), z1 = day_of(to - 1);
    int added = 0;
    for (int i = 0; i < n; i++) {
        if (!(e[i].show & where) || !own_valid(&e[i])) {
            continue;
        }
        for (long z = first_from(&e[i], z0); z >= 0 && z <= z1 && out->count < out->max;
             z = first_from(&e[i], z + 1)) {
            int64_t end;
            const int64_t start = midnight(z, &end);
            if (start >= to || end <= from) {
                continue;
            }
            int y, m, d;
            civil_from(z, &y, &m, &d);
            cal_event_t *c = &out->ev[out->count++];
            memset(c, 0, sizeof(*c));
            c->start = start;
            c->end = end;
            c->all_day = true;
            c->feed = OWN_FEED(e[i].kind);
            c->uid = (uint32_t)i;
            own_title(&e[i], y, c->title, sizeof(c->title));
            added++;
        }
    }
    return added;
}

int64_t own_next(const own_event_t *e, int64_t today)
{
    if (!own_valid(e)) {
        return 0;
    }
    const long z = first_from(e, day_of(today));
    int64_t end;
    return z >= 0 ? midnight(z, &end) : 0;
}

int own_prune(own_event_t *e, int n, int64_t today, int days)
{
    int kept = 0;
    for (int i = 0; i < n; i++) {
        /* By the calendar, so summer time doesn't move it. */
        if (!yearly(&e[i]) && e[i].repeat == OWN_ONCE &&
            civil_days(e[i].year, e[i].month, e[i].day) + 1 + days <= day_of(today)) {
            continue;
        }
        e[kept++] = e[i];
    }
    return kept;
}

cJSON *own_to_json(const own_event_t *e, int n)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; arr != NULL && i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(o, "kind", KIND_KEY[e[i].kind < OWN_KINDS ? e[i].kind : OWN_OTHER]);
        cJSON_AddStringToObject(o, "text", e[i].text);
        cJSON_AddNumberToObject(o, "day", e[i].day);
        cJSON_AddNumberToObject(o, "month", e[i].month);
        if (e[i].year != 0) {
            cJSON_AddNumberToObject(o, "year", e[i].year);
        }
        if (e[i].repeat > OWN_ONCE && e[i].repeat < OWN_REPEATS) {
            cJSON_AddStringToObject(o, "repeat", REPEAT_KEY[e[i].repeat]);
            if (e[i].every > 1) {
                cJSON_AddNumberToObject(o, "every", e[i].every);
            }
        }
        cJSON *show = cJSON_AddObjectToObject(o, "show");
        for (int b = 0; b < 3; b++) {
            cJSON_AddBoolToObject(show, SHOW_KEY[b], (e[i].show >> b) & 1);
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* A whole number in [lo, hi] under `key`, or -1. */
static int json_int(const cJSON *o, const char *key, int lo, int hi)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble != (int)v->valuedouble || v->valuedouble < lo || v->valuedouble > hi) {
        return -1;
    }
    return (int)v->valuedouble;
}

int own_from_json(const cJSON *arr, own_event_t *e, char *err, size_t err_len)
{
    if (!cJSON_IsArray(arr)) {
        snprintf(err, err_len, "Hendelsene i sikkerhetskopien er ikke en liste.");
        return -1;
    }
    const int n = cJSON_GetArraySize(arr);
    if (n > OWN_MAX) {
        snprintf(err, err_len, "Sikkerhetskopien har flere enn %d hendelser.", OWN_MAX);
        return -1;
    }
    int i = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, arr)
    {
        own_event_t *x = &e[i];
        memset(x, 0, sizeof(*x));
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(o, "kind");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(o, "text");
        const cJSON *show = cJSON_GetObjectItemCaseSensitive(o, "show");
        x->kind = OWN_KINDS;
        for (int k = 0; cJSON_IsString(kind) && k < OWN_KINDS; k++) {
            if (strcmp(kind->valuestring, KIND_KEY[k]) == 0) {
                x->kind = (uint8_t)k;
            }
        }
        const int day = json_int(o, "day", 1, 31), month = json_int(o, "month", 1, 12);
        const int year = cJSON_GetObjectItemCaseSensitive(o, "year") != NULL ? json_int(o, "year", 1900, 2200) : 0;
        const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(o, "repeat");
        int rep = repeat == NULL ? OWN_ONCE : -1;
        for (int r = OWN_WEEKLY; cJSON_IsString(repeat) && r < OWN_REPEATS; r++) {
            if (strcmp(repeat->valuestring, REPEAT_KEY[r]) == 0) {
                rep = r;
            }
        }
        const int every = rep <= OWN_ONCE                                       ? 0
                        : cJSON_GetObjectItemCaseSensitive(o, "every") == NULL ? 1
                                                                               : json_int(o, "every", 1, 255);
        bool ok = cJSON_IsString(text) && strlen(text->valuestring) < sizeof(x->text) && day > 0 && month > 0 &&
                  year >= 0 && rep >= 0 && every >= 0;
        if (ok) {
            snprintf(x->text, sizeof(x->text), "%s", text->valuestring);
            x->day = (uint8_t)day;
            x->month = (uint8_t)month;
            x->year = (uint16_t)year;
            x->repeat = (uint8_t)rep;
            x->every = (uint8_t)every;
            for (int b = 0; b < 3; b++) {
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(show, SHOW_KEY[b]))) {
                    x->show |= (uint8_t)(1 << b);
                }
            }
            ok = own_valid(x);
        }
        if (!ok) {
            snprintf(err, err_len, "Hendelse %d i sikkerhetskopien er ikke gyldig.", i + 1);
            return -1;
        }
        i++;
    }
    return n;
}

/* Keep only the valid ones of e[0..got-1]: how many. */
static int keep_valid(own_event_t *e, int got)
{
    int n = 0;
    for (int i = 0; i < got; i++) {
        if (own_valid(&e[i])) {
            e[n++] = e[i];
        }
    }
    if (n != got) {
        ESP_LOGW(TAG, "%d saved event(s) not valid, left out", got - n);
    }
    return n;
}

int own_load(own_event_t *e)
{
    nvs_handle_t h;
    if (nvs_open(OWN_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0; /* none saved yet */
    }
    size_t len = OWN_MAX * sizeof(own_event_t);
    int n = 0;
    if (nvs_get_blob(h, OWN_NVS_KEY, e, &len) == ESP_OK) {
        if (len % sizeof(own_event_t) == 0) {
            n = keep_valid(e, (int)(len / sizeof(own_event_t)));
        }
    } else {
        /* Saved before repeats: each one read as once. Saved as v2 (and
         * this erased) on the next change. */
        own_event_v1_t *old = malloc(OWN_MAX * sizeof(own_event_v1_t)); /* once, at start-up */
        len = OWN_MAX * sizeof(own_event_v1_t);
        if (old != NULL && nvs_get_blob(h, OWN_NVS_V1, old, &len) == ESP_OK && len % sizeof(own_event_v1_t) == 0) {
            const int got = (int)(len / sizeof(own_event_v1_t));
            for (int i = 0; i < got; i++) {
                memset(&e[i], 0, sizeof(e[i]));
                memcpy(e[i].text, old[i].text, sizeof(e[i].text));
                e[i].year = old[i].year;
                e[i].month = old[i].month;
                e[i].day = old[i].day;
                e[i].kind = old[i].kind;
                e[i].show = old[i].show;
            }
            n = keep_valid(e, got);
        }
        free(old);
    }
    nvs_close(h);
    return n;
}

esp_err_t own_save(const own_event_t *e, int n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(OWN_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = n > 0 ? nvs_set_blob(h, OWN_NVS_KEY, e, (size_t)n * sizeof(own_event_t)) : nvs_erase_key(h, OWN_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* nothing to erase */
    }
    if (err == ESP_OK) {
        const esp_err_t e1 = nvs_erase_key(h, OWN_NVS_V1); /* read into v2 by own_load */
        err = e1 == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e1;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
