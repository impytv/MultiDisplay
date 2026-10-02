#ifndef _ICAL_H_
#define _ICAL_H_

/* Reading calendars in iCalendar format (RFC 5545), as Google, Outlook and
 * iCloud publish them behind a secret address: the events of a time window,
 * recurring ones expanded.
 *
 * The file is read as it streams in (ical_feed), a line at a time, so a
 * calendar of several megabytes - years of history - needs no more than a
 * few KB. What is understood:
 *
 *  - DTSTART/DTEND/DURATION as dates (all-day), UTC times, or local times
 *    with or without a TZID. A TZID is taken to mean Norwegian time (only
 *    UTC is told apart): the zone rules in the file aren't read.
 *  - RRULE with FREQ DAILY, WEEKLY, MONTHLY or YEARLY, INTERVAL, COUNT,
 *    UNTIL, BYDAY (with an ordinal for monthly and yearly: 2TU, -1FR),
 *    BYMONTHDAY and BYMONTH. Not BYSETPOS, BYWEEKNO, BYYEARDAY or the
 *    hourly and finer frequencies.
 *  - EXDATE (dropped instances), RECURRENCE-ID (a moved or changed
 *    instance replaces the original), STATUS:CANCELLED. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAL_EVENTS_MAX 96
#define CAL_TITLE_MAX  80

typedef struct {
    int64_t start, end; /* epoch seconds; an all-day event runs from local midnight to the midnight after */
    uint32_t uid;       /* hash of the event's UID */
    uint8_t feed;       /* which calendar it came from */
    bool all_day;
    bool moved;         /* replaces one instance of a recurring event */
    char title[CAL_TITLE_MAX];
} cal_event_t;

/* Events from one or more calendars; the earliest are kept when there are
 * more than fit. */
typedef struct {
    int count;
    cal_event_t ev[CAL_EVENTS_MAX];
} cal_list_t;

typedef struct ical_parser ical_parser_t;

/* Start reading calendar number `feed` into `out`, keeping the events that
 * overlap [from, to). NULL if out of memory. */
ical_parser_t *ical_begin(cal_list_t *out, uint8_t feed, int64_t from, int64_t to);

/* The next `len` bytes of the file, in any size of piece. */
void ical_feed(ical_parser_t *p, const char *data, size_t len);

/* Finish (and free the parser): the number of events this calendar has in
 * `out`, or -1 if the file wasn't a calendar. */
int ical_end(ical_parser_t *p);

/* Order by start, all-day events first on a day. */
void cal_sort(cal_list_t *list);

#endif
