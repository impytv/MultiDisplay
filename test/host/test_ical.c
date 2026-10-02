#include <stdlib.h>
#include <time.h>
#include "check.h"
#include "../../components/ical.c"

/* Norwegian local time, as on the display. */
static int64_t mk(int y, int mo, int d, int h, int mi)
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi,
                    .tm_isdst = -1 };
    return (int64_t)mktime(&t);
}

static const cal_event_t *find(const cal_list_t *l, int64_t start)
{
    for (int i = 0; i < l->count; i++) {
        if (l->ev[i].start == start) {
            return &l->ev[i];
        }
    }
    return NULL;
}

/* A calendar as Google writes one: CRLF, folded lines, a time zone block,
 * escapes and an alarm inside an event. */
static const char CAL[] =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Google Inc//Google Calendar 70.9054//EN\r\n"
    "BEGIN:VTIMEZONE\r\nTZID:Europe/Oslo\r\nBEGIN:STANDARD\r\nDTSTART:19701025T030000\r\n"
    "TZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n"
    /* A: one appointment, its title folded and escaped. */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Oslo:20261006T093000\r\nDTEND;TZID=Europe/Oslo:20261006T103000\r\n"
    "UID:a@google.com\r\nSUMMARY:Tannl\r\n ege\\, kontroll\r\nEND:VEVENT\r\n"
    /* B: two days away, all day. */
    "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261010\r\nDTEND;VALUE=DATE:20261012\r\nUID:b\r\n"
    "SUMMARY:Hytta \xC3\xA6\xC3\xB8\xC3\xA5\r\nEND:VEVENT\r\n"
    /* C: every Monday and Wednesday since 2020; one dropped, one moved. */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Oslo:20200106T180000\r\nDTEND;TZID=Europe/Oslo:20200106T190000\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=MO,WE\r\nEXDATE;TZID=Europe/Oslo:20261007T180000\r\nUID:c\r\n"
    "SUMMARY:Trening\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:Alarm\r\nTRIGGER:-PT10M\r\n"
    "END:VALARM\r\nEND:VEVENT\r\n"
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Oslo:20261013T200000\r\nDTEND;TZID=Europe/Oslo:20261013T210000\r\n"
    "RECURRENCE-ID;TZID=Europe/Oslo:20261012T180000\r\nUID:c\r\nSUMMARY:Trening (flyttet)\r\nEND:VEVENT\r\n"
    /* D: the last Friday of each month. */
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Oslo:20250131T150000\r\nDURATION:PT2H\r\n"
    "RRULE:FREQ=MONTHLY;BYDAY=-1FR\r\nUID:d\r\nSUMMARY:Fredagspils\r\nEND:VEVENT\r\n"
    /* E: a birthday. */
    "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:19801020\r\nRRULE:FREQ=YEARLY\r\nUID:e\r\nSUMMARY:Bursdag\r\nEND:VEVENT\r\n"
    /* F: five mornings, in UTC. */
    "BEGIN:VEVENT\r\nDTSTART:20261003T070000Z\r\nDTEND:20261003T073000Z\r\nRRULE:FREQ=DAILY;COUNT=5\r\n"
    "UID:f\r\nSUMMARY:Kurs\r\nEND:VEVENT\r\n"
    /* G: cancelled. H: outside the window. I: ended by UNTIL before it. */
    "BEGIN:VEVENT\r\nDTSTART:20261008T100000Z\r\nUID:g\r\nSTATUS:CANCELLED\r\nSUMMARY:Avlyst\r\nEND:VEVENT\r\n"
    "BEGIN:VEVENT\r\nDTSTART:20261201T100000Z\r\nUID:h\r\nSUMMARY:Senere\r\nEND:VEVENT\r\n"
    "BEGIN:VEVENT\r\nDTSTART;TZID=Europe/Oslo:20260105T080000\r\nRRULE:FREQ=WEEKLY;UNTIL=20261001T060000Z\r\n"
    "UID:i\r\nSUMMARY:Slutt\r\nEND:VEVENT\r\n"
    "END:VCALENDAR\r\n";

void test_ical(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    static cal_list_t l;
    const int64_t from = mk(2026, 10, 5, 0, 0), to = mk(2026, 11, 2, 0, 0);

    /* Fed in small, odd pieces, as it streams in. */
    ical_parser_t *p = ical_begin(&l, 1, from, to);
    for (size_t i = 0; i < sizeof(CAL) - 1; i += 7) {
        ical_feed(p, CAL + i, sizeof(CAL) - 1 - i < 7 ? sizeof(CAL) - 1 - i : 7);
    }
    CHECK_INT(ical_end(p), 14);
    cal_sort(&l);

    const cal_event_t *e = find(&l, mk(2026, 10, 6, 9, 30));
    CHECK(e != NULL);
    if (e) {
        CHECK_STR(e->title, "Tannlege, kontroll");
        CHECK_INT(e->end, mk(2026, 10, 6, 10, 30));
        CHECK(!e->all_day && e->feed == 1);
    }
    e = find(&l, mk(2026, 10, 10, 0, 0));
    CHECK(e != NULL && e->all_day && e->end == mk(2026, 10, 12, 0, 0));
    CHECK(e != NULL && strcmp(e->title, "Hytta \xC3\xA6\xC3\xB8\xC3\xA5") == 0);

    /* C: 5, 14, 19, 21, 26 (after the change to winter time) and 28 October,
     * not the dropped 7th; the 12th moved to the 13th at 20:00. */
    static const int c_days[] = { 5, 14, 19, 21, 26, 28 };
    for (size_t i = 0; i < sizeof(c_days) / sizeof(c_days[0]); i++) {
        e = find(&l, mk(2026, 10, c_days[i], 18, 0));
        CHECK(e != NULL && strcmp(e->title, "Trening") == 0 && e->end == mk(2026, 10, c_days[i], 19, 0));
    }
    CHECK(find(&l, mk(2026, 10, 7, 18, 0)) == NULL);
    CHECK(find(&l, mk(2026, 10, 12, 18, 0)) == NULL);
    e = find(&l, mk(2026, 10, 13, 20, 0));
    CHECK(e != NULL && e->moved && strcmp(e->title, "Trening (flyttet)") == 0);

    e = find(&l, mk(2026, 10, 30, 15, 0));
    CHECK(e != NULL && e->end == mk(2026, 10, 30, 17, 0));
    e = find(&l, mk(2026, 10, 20, 0, 0));
    CHECK(e != NULL && e->all_day && strcmp(e->title, "Bursdag") == 0);
    /* F: the 3rd to the 7th at 07:00 UTC (09:00 here); three in the window. */
    for (int d = 5; d <= 7; d++) {
        CHECK(find(&l, mk(2026, 10, d, 9, 0)) != NULL);
    }
    CHECK(find(&l, mk(2026, 10, 8, 9, 0)) == NULL);
    for (int i = 0; i < l.count; i++) {
        CHECK(strcmp(l.ev[i].title, "Avlyst") != 0 && strcmp(l.ev[i].title, "Senere") != 0 &&
              strcmp(l.ev[i].title, "Slutt") != 0);
        CHECK(i == 0 || l.ev[i - 1].start <= l.ev[i].start);
    }

    /* A second calendar adds to the same list; an event going on when the
     * window opens is kept. Not a calendar: -1. */
    p = ical_begin(&l, 2, from, to);
    static const char CAL2[] = "BEGIN:VCALENDAR\nBEGIN:VEVENT\nDTSTART;VALUE=DATE:20261001\n"
                               "DTEND;VALUE=DATE:20261008\nSUMMARY:Ferie\nUID:x\nEND:VEVENT\nEND:VCALENDAR\n";
    ical_feed(p, CAL2, sizeof(CAL2) - 1);
    CHECK_INT(ical_end(p), 1);
    CHECK_INT(l.count, 15);
    p = ical_begin(&l, 0, from, to);
    ical_feed(p, "<html>Not found</html>", 22);
    CHECK_INT(ical_end(p), -1);

    /* A full list keeps the earliest events. */
    memset(&l, 0, sizeof(l));
    p = ical_begin(&l, 0, from, mk(2027, 2, 1, 0, 0));
    static const char DAILY[] = "BEGIN:VCALENDAR\nBEGIN:VEVENT\nDTSTART:20261005T120000Z\nRRULE:FREQ=DAILY;INTERVAL=1\n"
                                "SUMMARY:Hver dag\nUID:y\nEND:VEVENT\nBEGIN:VEVENT\nDTSTART:20261004T230000Z\n"
                                "RRULE:FREQ=HOURLY\nSUMMARY:Hver time\nUID:z\nEND:VEVENT\nEND:VCALENDAR\n";
    ical_feed(p, DAILY, sizeof(DAILY) - 1);
    /* 119 days and the unsupported hourly rule as its first instance only
     * (01:00 the 5th): the 96 earliest are kept. */
    CHECK_INT(ical_end(p), CAL_EVENTS_MAX);
    cal_sort(&l);
    CHECK_INT(l.ev[0].start, mk(2026, 10, 5, 1, 0));

    /* A long title is cut at a whole character. */
    char t[CAL_TITLE_MAX];
    char longer[200];
    memset(longer, 'x', CAL_TITLE_MAX - 2);
    snprintf(longer + CAL_TITLE_MAX - 2, sizeof(longer) - (CAL_TITLE_MAX - 2), "\xC3\xB8 slutt");
    set_title(t, longer);
    CHECK_INT(strlen(t), CAL_TITLE_MAX - 2);
    CHECK_INT(parse_duration("P1DT2H30M"), 86400 + 2 * 3600 + 30 * 60);
    CHECK_INT(parse_duration("P2W"), 14 * 86400);
}
