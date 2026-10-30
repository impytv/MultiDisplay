#ifndef _OWN_EVENTS_H_
#define _OWN_EVENTS_H_

/* The display's own events, typed in on its events page (/hendelser):
 * birthdays and anniversaries every year, appointments and other events on
 * one date, each shown on the screens ticked for it - the calendar, the
 * month calendar and the clock. Kept in NVS (namespace "hendelser"). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ical.h"

#define OWN_MAX      100
#define OWN_TEXT_MAX 64 /* bytes, with the NUL: the page asks for at most 40 characters */

typedef enum {
    OWN_BIRTHDAY,    /* every year; the year it is from (optional) gives the age */
    OWN_ANNIVERSARY, /* every year; the year (optional) gives how many years */
    OWN_APPOINTMENT, /* on one date */
    OWN_OTHER,       /* on one date */
    OWN_KINDS
} own_kind_t;

/* Where an event shows. */
#define OWN_ON_CALENDAR 0x01
#define OWN_ON_MONTH    0x02
#define OWN_ON_CLOCK    0x04
#define OWN_ON_ALL      0x07

typedef struct {
    char text[OWN_TEXT_MAX]; /* a birthday's: who */
    uint16_t year;           /* 0 = not given (only for those every year) */
    uint8_t month, day;      /* 1-12, 1-31 */
    uint8_t kind;            /* own_kind_t */
    uint8_t show;            /* OWN_ON_* */
} own_event_t;

/* cal_event_t.feed for an event of kind k: past the calendars' own. */
#define OWN_FEED(k)    (16 + (k))
#define OWN_IS_FEED(f) ((f) >= 16 && (f) < 16 + OWN_KINDS)

/* The kind's name ("Bursdag") and colour (0xRRGGBB). */
const char *own_kind_name(int kind);
uint32_t own_kind_colour(int kind);

/* Whether e is a whole event: text, a date that exists (29 February
 * without a year, for every year), a year where needed (1900-2200), and
 * somewhere to show it. */
bool own_valid(const own_event_t *e);

/* The text shown for e in `year`: "Kari fyller 80", "Kari har bursdag",
 * "Bryllupsdag (25 år)" or the text as typed. */
void own_title(const own_event_t *e, int year, char *dst, size_t len);

/* Append to `out` (as far as it has room) e[0..n-1]'s days that meet
 * [from, to) - local midnight to midnight - for those whose show has a bit
 * of `where`: all-day, feed OWN_FEED(kind), titled by own_title. 29
 * February is on the 28th in other years. The number appended. */
int own_days(const own_event_t *e, int n, int64_t from, int64_t to, uint8_t where, cal_list_t *out);

/* The next day e is on, today included (local midnight), or 0 if none -
 * a date passed. */
int64_t own_next(const own_event_t *e, int64_t today);

/* The events as a JSON array for the settings backup:
 *   [{"kind":"birthday","text":"Kari","day":12,"month":11,"year":1946,
 *     "show":{"calendar":true,"month":true,"clock":true}}, ...]
 * kind is birthday, anniversary, appointment or other; year left out when
 * not given. NULL if out of memory. */
struct cJSON;
struct cJSON *own_to_json(const own_event_t *e, int n);

/* Read such an array into e (room for OWN_MAX): how many, or -1 with a
 * Norwegian reason in `err` if it isn't one or an event isn't whole. */
int own_from_json(const struct cJSON *arr, own_event_t *e, char *err, size_t err_len);

/* Load into e (room for OWN_MAX) from NVS: how many. */
int own_load(own_event_t *e);

/* Save e[0..n-1] to NVS. */
esp_err_t own_save(const own_event_t *e, int n);

#endif
