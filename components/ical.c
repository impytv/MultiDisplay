/* iCalendar reading (see ical.h). Dates are worked with as day numbers
 * (days since 1970-01-01, civil_time.h) so recurrences step over months and
 * years exactly, and turned into times with mktime(), so an event at 10:00
 * stays at 10:00 across the change to and from summer time. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "civil_time.h"
#include "ical.h"

#define LINE_MAX_LEN 1024 /* longer lines (descriptions) are cut; nothing needed is in them */
#define EXDATES_MAX  48
#define MOVED_MAX    128
#define RRULE_MAX    256
#define PERIODS_MAX  200000 /* daily for 500 years, as a stop for a broken rule */

enum { K_DATE, K_LOCAL, K_UTC };

typedef struct {
    int64_t day; /* days since 1970-01-01 */
    int sec;     /* seconds into the day */
    uint8_t kind;
} ical_time_t;

typedef struct {
    uint32_t uid;
    int64_t start;
} moved_t;

struct ical_parser {
    cal_list_t *out;
    uint8_t feed;
    int64_t from, to;
    char line[LINE_MAX_LEN];
    size_t len;
    bool eol;      /* a line ended; the next byte says whether it goes on (folding) */
    bool calendar; /* seen BEGIN:VCALENDAR */
    /* The event being read. */
    bool in_event;
    int nested; /* depth of components inside it (VALARM) */
    bool has_start, has_end, has_recid, cancelled;
    ical_time_t start, end, recid;
    int64_t duration; /* seconds, -1 = none */
    uint32_t uid;
    char summary[CAL_TITLE_MAX];
    char rrule[RRULE_MAX];
    ical_time_t exdate[EXDATES_MAX];
    int exdates;
    /* Instances replaced by a moved one, removed at the end. */
    moved_t moved[MOVED_MAX];
    int moved_count;
};

/* --------------------------------------------------------------------------
 * Dates
 * ------------------------------------------------------------------------ */

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* 0 = Monday. 1970-01-01 was a Thursday. */
static int weekday(int64_t day)
{
    return (int)(((day % 7) + 7 + 3) % 7);
}

static int days_in_month(int y, int m)
{
    static const int D[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return D[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
}

/* Epoch seconds of `sec` into day `day`, in Norwegian time unless UTC. */
static int64_t at(int64_t day, int sec, uint8_t kind)
{
    if (kind == K_UTC) {
        return day * 86400 + sec;
    }
    int y, m, d;
    civil_from_days(day, &y, &m, &d);
    struct tm t = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = sec / 3600,
                    .tm_min = sec / 60 % 60, .tm_sec = sec % 60, .tm_isdst = -1 };
    return (int64_t)mktime(&t);
}

static int64_t epoch_of(const ical_time_t *t)
{
    return at(t->day, t->sec, t->kind);
}

/* "20261002", "20261002T100000" or "20261002T080000Z". */
static bool parse_time(const char *v, bool utc_zone, ical_time_t *t)
{
    int y, m, d, hh = 0, mm = 0, ss = 0;
    if (sscanf(v, "%4d%2d%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    t->day = days_from_civil(y, m, d);
    t->kind = K_DATE;
    t->sec = 0;
    if (v[8] == 'T') {
        if (sscanf(v + 9, "%2d%2d%2d", &hh, &mm, &ss) != 3) {
            return false;
        }
        t->sec = hh * 3600 + mm * 60 + ss;
        t->kind = (v[15] == 'Z' || utc_zone) ? K_UTC : K_LOCAL;
    }
    return true;
}

/* "PT1H30M", "P1D", "P2W", "P1DT12H". */
static int64_t parse_duration(const char *v)
{
    if (*v == '+' || *v == '-') {
        v++;
    }
    if (*v++ != 'P') {
        return -1;
    }
    int64_t s = 0;
    long n = 0;
    for (; *v; v++) {
        if (*v >= '0' && *v <= '9') {
            n = n * 10 + (*v - '0');
            continue;
        }
        switch (*v) {
        case 'W': s += n * 7 * 86400; break;
        case 'D': s += n * 86400; break;
        case 'H': s += n * 3600; break;
        case 'M': s += n * 60; break;
        case 'S': s += n; break;
        default: break; /* 'T' */
        }
        n = 0;
    }
    return s;
}

/* --------------------------------------------------------------------------
 * Events
 * ------------------------------------------------------------------------ */

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

/* SUMMARY text: escapes undone, cut at a whole UTF-8 character. */
static void set_title(char *dst, const char *v)
{
    char tmp[CAL_TITLE_MAX * 2];
    size_t n = 0;
    while (*v == ' ') {
        v++;
    }
    for (; *v && n < sizeof(tmp) - 1; v++) {
        char c = *v;
        if (c == '\\' && v[1] != '\0') {
            c = *++v;
            if (c == 'n' || c == 'N') {
                c = ' ';
            }
        }
        tmp[n++] = c;
    }
    tmp[n] = '\0';
    if (n > CAL_TITLE_MAX - 1) {
        n = CAL_TITLE_MAX - 1; /* tmp[n] is the first byte left out: not one inside a character */
        while (n > 0 && ((unsigned char)tmp[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    while (n > 0 && tmp[n - 1] == ' ') {
        n--;
    }
    memcpy(dst, tmp, n);
    dst[n] = '\0';
}

static int cmp_event(const void *a, const void *b)
{
    const cal_event_t *x = a, *y = b;
    if (x->start != y->start) {
        return x->start < y->start ? -1 : 1;
    }
    return (int)y->all_day - (int)x->all_day;
}

void cal_sort(cal_list_t *list)
{
    qsort(list->ev, (size_t)list->count, sizeof(list->ev[0]), cmp_event);
}

/* Keep an instance that meets the window; when the list is full, the
 * latest goes to make room for an earlier one. */
static void emit(ical_parser_t *p, int64_t start, int64_t end, bool all_day, bool moved)
{
    const bool meets = start < p->to && (end > p->from || (end == start && start >= p->from));
    if (!meets) {
        return;
    }
    cal_list_t *l = p->out;
    int slot = l->count;
    if (l->count == CAL_EVENTS_MAX) {
        slot = 0;
        for (int i = 1; i < l->count; i++) {
            if (l->ev[i].start > l->ev[slot].start) {
                slot = i;
            }
        }
        if (l->ev[slot].start <= start) {
            return;
        }
    } else {
        l->count++;
    }
    cal_event_t *e = &l->ev[slot];
    e->start = start;
    e->end = end;
    e->uid = p->uid;
    e->feed = p->feed;
    e->all_day = all_day;
    e->moved = moved;
    snprintf(e->title, sizeof(e->title), "%s", p->summary[0] ? p->summary : "(uten tittel)");
}

typedef struct {
    enum { F_NONE, F_DAILY, F_WEEKLY, F_MONTHLY, F_YEARLY } freq;
    int interval;
    int count; /* 0 = no limit */
    bool has_until;
    int64_t until;     /* epoch seconds, inclusive */
    int64_t until_day; /* the day it falls on, give or take one */
    int nby;
    int8_t by_wd[14], by_ord[14];
    int nmd;
    int8_t by_md[31];
    uint16_t months; /* bit m for BYMONTH m, 0 = any */
} rule_t;

static bool parse_rule(const char *s, rule_t *r)
{
    memset(r, 0, sizeof(*r));
    r->interval = 1;
    char buf[RRULE_MAX];
    snprintf(buf, sizeof(buf), "%s", s);
    char *save = NULL;
    for (char *part = strtok_r(buf, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        char *v = strchr(part, '=');
        if (v == NULL) {
            continue;
        }
        *v++ = '\0';
        if (strcasecmp(part, "FREQ") == 0) {
            r->freq = strcasecmp(v, "DAILY") == 0     ? F_DAILY
                      : strcasecmp(v, "WEEKLY") == 0  ? F_WEEKLY
                      : strcasecmp(v, "MONTHLY") == 0 ? F_MONTHLY
                      : strcasecmp(v, "YEARLY") == 0  ? F_YEARLY
                                                      : F_NONE;
        } else if (strcasecmp(part, "INTERVAL") == 0) {
            r->interval = atoi(v) > 0 ? atoi(v) : 1;
        } else if (strcasecmp(part, "COUNT") == 0) {
            r->count = atoi(v) > 0 ? atoi(v) : 0;
        } else if (strcasecmp(part, "UNTIL") == 0) {
            ical_time_t u;
            if (parse_time(v, false, &u)) {
                r->has_until = true;
                r->until = u.kind == K_DATE ? at(u.day + 1, 0, K_LOCAL) - 1 : epoch_of(&u);
                r->until_day = u.day;
            }
        } else if (strcasecmp(part, "BYDAY") == 0) {
            static const char *const WD[] = { "MO", "TU", "WE", "TH", "FR", "SA", "SU" };
            char *save2 = NULL;
            for (char *d = strtok_r(v, ",", &save2); d && r->nby < 14; d = strtok_r(NULL, ",", &save2)) {
                const int n = (int)strlen(d);
                if (n < 2) {
                    continue;
                }
                for (int w = 0; w < 7; w++) {
                    if (strcasecmp(d + n - 2, WD[w]) == 0) {
                        d[n - 2] = '\0';
                        r->by_wd[r->nby] = (int8_t)w;
                        r->by_ord[r->nby] = (int8_t)atoi(d);
                        r->nby++;
                    }
                }
            }
        } else if (strcasecmp(part, "BYMONTHDAY") == 0) {
            char *save2 = NULL;
            for (char *d = strtok_r(v, ",", &save2); d && r->nmd < 31; d = strtok_r(NULL, ",", &save2)) {
                const int md = atoi(d);
                if (md != 0 && md >= -31 && md <= 31) {
                    r->by_md[r->nmd++] = (int8_t)md;
                }
            }
        } else if (strcasecmp(part, "BYMONTH") == 0) {
            char *save2 = NULL;
            for (char *d = strtok_r(v, ",", &save2); d; d = strtok_r(NULL, ",", &save2)) {
                const int m = atoi(d);
                if (m >= 1 && m <= 12) {
                    r->months |= (uint16_t)(1u << m);
                }
            }
        }
    }
    return r->freq != F_NONE;
}

/* The days of month y-m that the rule picks, added to c[]. */
static int month_days(const rule_t *r, int y, int m, int start_mday, int64_t *c, int n)
{
    const int64_t first = days_from_civil(y, m, 1);
    const int dim = days_in_month(y, m);
    if (r->nby > 0) {
        for (int i = 0; i < r->nby && n < 62; i++) {
            const int wd = r->by_wd[i], ord = r->by_ord[i];
            const int64_t first_wd = first + (wd - weekday(first) + 7) % 7;
            if (ord == 0) {
                for (int64_t d = first_wd; d < first + dim && n < 62; d += 7) {
                    c[n++] = d;
                }
            } else if (ord > 0) {
                const int64_t d = first_wd + 7 * (ord - 1);
                if (d < first + dim) {
                    c[n++] = d;
                }
            } else {
                const int64_t last = first + dim - 1;
                const int64_t d = last - (weekday(last) - wd + 7) % 7 + 7 * (ord + 1);
                if (d >= first) {
                    c[n++] = d;
                }
            }
        }
    } else if (r->nmd > 0) {
        for (int i = 0; i < r->nmd && n < 62; i++) {
            const int md = r->by_md[i] > 0 ? r->by_md[i] : dim + 1 + r->by_md[i];
            if (md >= 1 && md <= dim) {
                c[n++] = first + md - 1;
            }
        }
    } else if (start_mday <= dim) {
        c[n++] = first + start_mday - 1;
    }
    return n;
}

static bool excluded(const ical_parser_t *p, int64_t day, int64_t start)
{
    for (int i = 0; i < p->exdates; i++) {
        const ical_time_t *x = &p->exdate[i];
        if (x->kind == K_DATE ? x->day == day : epoch_of(x) == start) {
            return true;
        }
    }
    return false;
}

static int cmp_day(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* Every instance of the event being read, from its rule. */
static void expand(ical_parser_t *p, const rule_t *r, int64_t dur_days, int64_t dur_s)
{
    const ical_time_t *s = &p->start;
    const bool all_day = s->kind == K_DATE;
    int sy, sm, sd;
    civil_from_days(s->day, &sy, &sm, &sd);
    /* The window in days, a day wider on each side for the time zone. */
    const int64_t from_day = p->from / 86400 - 1, to_day = p->to / 86400 + 1;
    const int64_t monday0 = s->day - weekday(s->day);
    const int64_t span_days = all_day ? dur_days : dur_s / 86400 + 1; /* how far an instance reaches */
    int counted = 0;

    for (long k = 0; k < PERIODS_MAX; k++) {
        int64_t c[62];
        int n = 0;
        int64_t period_first;
        const long step = k * r->interval;
        if (r->freq == F_DAILY) {
            period_first = s->day + step;
            c[n++] = period_first;
        } else if (r->freq == F_WEEKLY) {
            period_first = monday0 + 7 * step;
            if (r->nby == 0) {
                c[n++] = period_first + weekday(s->day);
            }
            for (int i = 0; i < r->nby; i++) {
                c[n++] = period_first + r->by_wd[i];
            }
        } else {
            const long months = r->freq == F_MONTHLY ? step : 12 * step;
            const int y = sy + (int)((sm - 1 + months) / 12), m = (int)((sm - 1 + months) % 12) + 1;
            period_first = days_from_civil(y, m, 1);
            if (r->freq == F_MONTHLY) {
                n = month_days(r, y, m, sd, c, n);
            } else {
                for (int mm = 1; mm <= 12; mm++) {
                    if (r->months ? (r->months & (1u << mm)) : mm == sm) {
                        n = month_days(r, y, mm, sd, c, n);
                    }
                }
            }
        }
        if (period_first > to_day) {
            return;
        }
        qsort(c, (size_t)n, sizeof(c[0]), cmp_day);
        for (int i = 0; i < n; i++) {
            const int64_t day = c[i];
            if (day < s->day || (i > 0 && day == c[i - 1])) {
                continue;
            }
            int y, m, d;
            civil_from_days(day, &y, &m, &d);
            if (r->months && r->freq != F_YEARLY && !(r->months & (1u << m))) {
                continue;
            }
            if (r->freq == F_DAILY && r->nby > 0) {
                bool hit = false;
                for (int b = 0; b < r->nby; b++) {
                    hit |= r->by_wd[b] == weekday(day);
                }
                if (!hit) {
                    continue;
                }
            }
            /* Far before the window only the count matters (and UNTIL, by
             * day): no need to work out the time. */
            if (day + span_days < from_day && (!r->has_until || day + 1 < r->until_day)) {
                if (r->count && ++counted >= r->count) {
                    return;
                }
                continue;
            }
            const int64_t start = at(day, s->sec, all_day ? K_LOCAL : s->kind);
            if (r->has_until && start > r->until) {
                return;
            }
            if (r->count && ++counted > r->count) {
                return;
            }
            if (start >= p->to) {
                return;
            }
            if (!excluded(p, day, start)) {
                const int64_t end = all_day ? at(day + dur_days, 0, K_LOCAL) : start + dur_s;
                emit(p, start, end, all_day, false);
            }
        }
    }
}

static void finish_event(ical_parser_t *p)
{
    if (!p->has_start) {
        return;
    }
    const bool all_day = p->start.kind == K_DATE;
    int64_t dur_days = 1, dur_s = 0;
    if (p->has_end) {
        if (all_day) {
            dur_days = p->end.day - p->start.day;
        } else {
            dur_s = epoch_of(&p->end) - epoch_of(&p->start);
        }
    } else if (p->duration >= 0) {
        if (all_day) {
            dur_days = p->duration / 86400;
        } else {
            dur_s = p->duration;
        }
    }
    dur_days = dur_days < 1 ? 1 : dur_days;
    dur_s = dur_s < 0 ? 0 : dur_s;

    /* A moved or changed instance: the original goes, this one stands in. */
    if (p->has_recid) {
        if (p->moved_count < MOVED_MAX) {
            p->moved[p->moved_count++] = (moved_t){ p->uid, epoch_of(&p->recid) };
        }
        if (p->cancelled) {
            return;
        }
        const int64_t start = epoch_of(&p->start);
        emit(p, start, all_day ? at(p->start.day + dur_days, 0, K_LOCAL) : start + dur_s, all_day, true);
        return;
    }
    if (p->cancelled) {
        return;
    }
    rule_t r;
    if (p->rrule[0] != '\0' && parse_rule(p->rrule, &r)) {
        expand(p, &r, dur_days, dur_s);
        return;
    }
    const int64_t start = epoch_of(&p->start);
    if (!excluded(p, p->start.day, start)) {
        emit(p, start, all_day ? at(p->start.day + dur_days, 0, K_LOCAL) : start + dur_s, all_day, false);
    }
}

/* --------------------------------------------------------------------------
 * Lines
 * ------------------------------------------------------------------------ */

/* Parameter `name` (TZID, VALUE) of a property, into dst; false if absent. */
static bool param(const char *params, const char *name, char *dst, size_t len)
{
    const size_t n = strlen(name);
    for (const char *p = params; p != NULL && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        if (strncasecmp(p, name, n) == 0 && p[n] == '=') {
            const char *v = p + n + 1;
            const bool quoted = *v == '"';
            v += quoted;
            size_t k = 0;
            while (v[k] && v[k] != (quoted ? '"' : ';') && k < len - 1) {
                k++;
            }
            memcpy(dst, v, k);
            dst[k] = '\0';
            return true;
        }
    }
    return false;
}

static bool utc_zone(const char *params)
{
    char tz[48];
    if (!param(params, "TZID", tz, sizeof(tz))) {
        return false;
    }
    return strcasecmp(tz, "UTC") == 0 || strcasecmp(tz, "Etc/UTC") == 0 || strcasecmp(tz, "GMT") == 0 ||
           strcasecmp(tz, "Etc/GMT") == 0 || strcasecmp(tz, "Z") == 0;
}

static void reset_event(ical_parser_t *p)
{
    p->in_event = true;
    p->nested = 0;
    p->has_start = p->has_end = p->has_recid = p->cancelled = false;
    p->duration = -1;
    p->uid = 0;
    p->summary[0] = p->rrule[0] = '\0';
    p->exdates = 0;
}

static void handle_line(ical_parser_t *p)
{
    char *line = p->line;
    char *colon = NULL;
    bool quoted = false;
    for (char *c = line; *c; c++) {
        if (*c == '"') {
            quoted = !quoted;
        } else if (*c == ':' && !quoted) {
            colon = c;
            break;
        }
    }
    if (colon == NULL) {
        return;
    }
    *colon = '\0';
    const char *value = colon + 1;
    char *params = strchr(line, ';');
    if (params != NULL) {
        *params++ = '\0';
    }
    const char *name = line;

    if (strcasecmp(name, "BEGIN") == 0) {
        if (strcasecmp(value, "VCALENDAR") == 0) {
            p->calendar = true;
        } else if (p->in_event) {
            p->nested++;
        } else if (strcasecmp(value, "VEVENT") == 0) {
            reset_event(p);
        }
        return;
    }
    if (strcasecmp(name, "END") == 0) {
        if (p->in_event && p->nested > 0) {
            p->nested--;
        } else if (p->in_event && strcasecmp(value, "VEVENT") == 0) {
            finish_event(p);
            p->in_event = false;
        }
        return;
    }
    if (!p->in_event || p->nested > 0) {
        return;
    }
    const bool utc = params != NULL && utc_zone(params);
    if (strcasecmp(name, "SUMMARY") == 0) {
        set_title(p->summary, value);
    } else if (strcasecmp(name, "UID") == 0) {
        p->uid = hash(value);
    } else if (strcasecmp(name, "DTSTART") == 0) {
        p->has_start = parse_time(value, utc, &p->start);
    } else if (strcasecmp(name, "DTEND") == 0) {
        p->has_end = parse_time(value, utc, &p->end);
    } else if (strcasecmp(name, "DURATION") == 0) {
        p->duration = parse_duration(value);
    } else if (strcasecmp(name, "RRULE") == 0) {
        snprintf(p->rrule, sizeof(p->rrule), "%s", value);
    } else if (strcasecmp(name, "RECURRENCE-ID") == 0) {
        p->has_recid = parse_time(value, utc, &p->recid);
    } else if (strcasecmp(name, "STATUS") == 0) {
        p->cancelled = strcasecmp(value, "CANCELLED") == 0;
    } else if (strcasecmp(name, "EXDATE") == 0) {
        char *save = NULL; /* the value is ours to cut up: it's in p->line */
        for (char *v = strtok_r((char *)value, ",", &save); v && p->exdates < EXDATES_MAX; v = strtok_r(NULL, ",", &save)) {
            if (parse_time(v, utc, &p->exdate[p->exdates])) {
                p->exdates++;
            }
        }
    }
}

static void end_line(ical_parser_t *p)
{
    p->line[p->len] = '\0';
    handle_line(p);
    p->len = 0;
}

ical_parser_t *ical_begin(cal_list_t *out, uint8_t feed, int64_t from, int64_t to)
{
    ical_parser_t *p = calloc(1, sizeof(*p));
    if (p != NULL) {
        p->out = out;
        p->feed = feed;
        p->from = from;
        p->to = to;
    }
    return p;
}

void ical_feed(ical_parser_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (p->eol) {
            p->eol = false;
            if (c == ' ' || c == '\t') {
                continue; /* folded: the line goes on */
            }
            end_line(p);
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            p->eol = true;
            continue;
        }
        if (p->len < LINE_MAX_LEN - 1) {
            p->line[p->len++] = c;
        }
    }
}

int ical_end(ical_parser_t *p)
{
    if (p->eol || p->len > 0) {
        end_line(p);
    }
    cal_list_t *l = p->out;
    int n = 0;
    for (int i = 0; i < l->count;) {
        cal_event_t *e = &l->ev[i];
        bool gone = false;
        for (int m = 0; e->feed == p->feed && !e->moved && m < p->moved_count && !gone; m++) {
            gone = p->moved[m].uid == e->uid && p->moved[m].start == e->start;
        }
        if (gone) {
            *e = l->ev[--l->count];
            continue;
        }
        n += e->feed == p->feed;
        i++;
    }
    const int result = p->calendar ? n : -1;
    free(p);
    return result;
}
