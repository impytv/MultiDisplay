/* The display's own events (own_events.h): when each is on, its text, and
 * keeping them in NVS. */

#include "own_events.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "own_events";

#define OWN_NVS_NS  "hendelser"
#define OWN_NVS_KEY "v1" /* the layout of own_event_t: a new key if it changes */

/* In the backup. */
static const char *const KIND_KEY[OWN_KINDS] = { "birthday", "anniversary", "appointment", "other" };
static const char *const SHOW_KEY[3] = { "calendar", "month", "clock" };

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

/* Local midnight starting e's day in `year` (29 February on the 28th in
 * other years), and the midnight after. */
static int64_t day_in(const own_event_t *e, int year, int64_t *end)
{
    const int d = e->month == 2 && e->day == 29 && !leap(year) ? 28 : e->day;
    struct tm t = { .tm_year = year - 1900, .tm_mon = e->month - 1, .tm_mday = d, .tm_isdst = -1 };
    const int64_t start = (int64_t)mktime(&t);
    struct tm u = { .tm_year = year - 1900, .tm_mon = e->month - 1, .tm_mday = d + 1, .tm_isdst = -1 };
    *end = (int64_t)mktime(&u);
    return start;
}

static int year_of(int64_t t)
{
    const time_t tt = (time_t)t;
    struct tm lt;
    localtime_r(&tt, &lt);
    return lt.tm_year + 1900;
}

int own_days(const own_event_t *e, int n, int64_t from, int64_t to, uint8_t where, cal_list_t *out)
{
    if (to <= from) {
        return 0;
    }
    const int y0 = year_of(from), y1 = year_of(to - 1);
    int added = 0;
    for (int i = 0; i < n; i++) {
        if (!(e[i].show & where) || !own_valid(&e[i])) {
            continue;
        }
        for (int y = yearly(&e[i]) ? y0 : e[i].year; y <= (yearly(&e[i]) ? y1 : e[i].year); y++) {
            if (e[i].year != 0 && y < e[i].year) {
                continue;
            }
            int64_t end;
            const int64_t start = day_in(&e[i], y, &end);
            if (start >= to || end <= from || out->count >= out->max) {
                continue;
            }
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
    int64_t end;
    if (!yearly(e)) {
        const int64_t start = day_in(e, e->year, &end);
        return end > today ? start : 0;
    }
    int y = year_of(today);
    if (e->year > y) {
        y = e->year;
    }
    int64_t start = day_in(e, y, &end);
    if (end <= today) {
        start = day_in(e, y + 1, &end);
    }
    return start;
}

int own_prune(own_event_t *e, int n, int64_t today, int days)
{
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (!yearly(&e[i])) {
            int64_t end;
            day_in(&e[i], e[i].year, &end);
            const time_t tt = (time_t)end;
            struct tm t;
            localtime_r(&tt, &t);
            t.tm_mday += days; /* by the calendar, so summer time doesn't move it */
            t.tm_isdst = -1;
            if ((int64_t)mktime(&t) <= today) {
                continue;
            }
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
        bool ok = cJSON_IsString(text) && strlen(text->valuestring) < sizeof(x->text) && day > 0 && month > 0 &&
                  year >= 0;
        if (ok) {
            snprintf(x->text, sizeof(x->text), "%s", text->valuestring);
            x->day = (uint8_t)day;
            x->month = (uint8_t)month;
            x->year = (uint16_t)year;
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

int own_load(own_event_t *e)
{
    nvs_handle_t h;
    if (nvs_open(OWN_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0; /* none saved yet */
    }
    size_t len = OWN_MAX * sizeof(own_event_t);
    int n = 0;
    if (nvs_get_blob(h, OWN_NVS_KEY, e, &len) == ESP_OK && len % sizeof(own_event_t) == 0) {
        const int got = (int)(len / sizeof(own_event_t));
        for (int i = 0; i < got; i++) {
            if (own_valid(&e[i])) {
                e[n++] = e[i];
            }
        }
        if (n != got) {
            ESP_LOGW(TAG, "%d saved event(s) not valid, left out", got - n);
        }
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
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
