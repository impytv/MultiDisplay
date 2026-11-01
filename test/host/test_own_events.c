#include <stdlib.h>
#include <time.h>
#include "check.h"
#include "../../components/own_events.c"
#include "app_config.h"

static int64_t day0(int y, int m, int d)
{
    struct tm t = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_isdst = -1 };
    return (int64_t)mktime(&t);
}

static own_event_t ev(const char *text, int kind, int y, int m, int d, int show)
{
    own_event_t e = { .year = (uint16_t)y, .month = (uint8_t)m, .day = (uint8_t)d, .kind = (uint8_t)kind,
                      .show = (uint8_t)show };
    snprintf(e.text, sizeof(e.text), "%s", text);
    return e;
}

void test_own_events(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    /* What is a whole event. */
    own_event_t e = ev("Kari", OWN_BIRTHDAY, 1946, 11, 12, OWN_ON_ALL);
    CHECK(own_valid(&e));
    e.year = 0;
    CHECK(own_valid(&e));                                          /* the year is optional for birthdays */
    e = ev("Tannlege", OWN_APPOINTMENT, 0, 11, 12, OWN_ON_ALL);
    CHECK(!own_valid(&e));                                         /* but needed on one date */
    e = ev("Kari", OWN_BIRTHDAY, 0, 2, 29, OWN_ON_ALL);
    CHECK(own_valid(&e));                                          /* 29 February, any year */
    e = ev("Kari", OWN_BIRTHDAY, 1946, 2, 29, OWN_ON_ALL);
    CHECK(!own_valid(&e));                                         /* not in 1946 */
    e = ev("Kari", OWN_BIRTHDAY, 1948, 2, 29, OWN_ON_ALL);
    CHECK(own_valid(&e));
    e = ev("Kari", OWN_BIRTHDAY, 1946, 4, 31, OWN_ON_ALL);
    CHECK(!own_valid(&e));
    e = ev("Kari", OWN_BIRTHDAY, 1946, 4, 30, 0);
    CHECK(!own_valid(&e));                                         /* shown nowhere */
    e = ev("", OWN_OTHER, 2026, 4, 30, OWN_ON_ALL);
    CHECK(!own_valid(&e));
    e = ev("x", OWN_KINDS, 2026, 4, 30, OWN_ON_ALL);
    CHECK(!own_valid(&e));

    /* The text shown. */
    char t[CAL_TITLE_MAX];
    e = ev("Kari", OWN_BIRTHDAY, 1946, 11, 12, OWN_ON_ALL);
    own_title(&e, 2026, t, sizeof(t));
    CHECK_STR(t, "Kari fyller 80");
    own_title(&e, 1946, t, sizeof(t));
    CHECK_STR(t, "Kari har bursdag");
    e.year = 0;
    own_title(&e, 2026, t, sizeof(t));
    CHECK_STR(t, "Kari har bursdag");
    e = ev("Bryllupsdag", OWN_ANNIVERSARY, 2001, 6, 16, OWN_ON_ALL);
    own_title(&e, 2026, t, sizeof(t));
    CHECK_STR(t, "Bryllupsdag (25 \xC3\xA5r)");
    e = ev("Tannlege", OWN_APPOINTMENT, 2026, 11, 3, OWN_ON_ALL);
    own_title(&e, 2026, t, sizeof(t));
    CHECK_STR(t, "Tannlege");

    /* The days in a window, each year for the yearly ones. */
    own_event_t list[] = {
        ev("Kari", OWN_BIRTHDAY, 1946, 11, 12, OWN_ON_ALL),
        ev("Tannlege", OWN_APPOINTMENT, 2026, 11, 3, OWN_ON_CALENDAR),
        ev("Gammel", OWN_OTHER, 2025, 11, 5, OWN_ON_ALL),
        ev("Skuddår", OWN_BIRTHDAY, 2000, 2, 29, OWN_ON_MONTH),
        ev("Senere", OWN_BIRTHDAY, 2030, 11, 20, OWN_ON_ALL),
    };
    cal_list_t *out = calloc(1, CAL_LIST_SIZE(8));
    out->max = 8;
    CHECK_INT(own_days(list, 5, day0(2026, 11, 1), day0(2026, 12, 1), OWN_ON_ALL, out), 2);
    CHECK_INT(out->ev[0].start, day0(2026, 11, 12));
    CHECK_INT(out->ev[0].end, day0(2026, 11, 13));
    CHECK(out->ev[0].all_day && out->ev[0].feed == OWN_FEED(OWN_BIRTHDAY));
    CHECK_STR(out->ev[0].title, "Kari fyller 80");
    CHECK_INT(out->ev[1].start, day0(2026, 11, 3));
    out->count = 0;
    CHECK_INT(own_days(list, 5, day0(2026, 11, 1), day0(2026, 12, 1), OWN_ON_CLOCK, out), 1); /* not the dentist */
    /* Across new year: one a year. */
    out->count = 0;
    CHECK_INT(own_days(list, 1, day0(2026, 11, 1), day0(2028, 12, 1), OWN_ON_ALL, out), 3);
    CHECK_STR(out->ev[2].title, "Kari fyller 82");
    /* 29 February on the 28th in other years. */
    out->count = 0;
    CHECK_INT(own_days(&list[3], 1, day0(2027, 2, 1), day0(2027, 3, 1), OWN_ON_MONTH, out), 1);
    CHECK_INT(out->ev[0].start, day0(2027, 2, 28));
    CHECK_STR(out->ev[0].title, "Skuddår fyller 27");
    out->count = 0;
    CHECK_INT(own_days(&list[3], 1, day0(2028, 2, 1), day0(2028, 3, 1), OWN_ON_MONTH, out), 1);
    CHECK_INT(out->ev[0].start, day0(2028, 2, 29));
    /* A full list takes no more. */
    out->count = out->max;
    CHECK_INT(own_days(list, 5, day0(2026, 11, 1), day0(2026, 12, 1), OWN_ON_ALL, out), 0);
    free(out);

    /* The next day each is on. */
    CHECK_INT(own_next(&list[0], day0(2026, 10, 30)), day0(2026, 11, 12));
    CHECK_INT(own_next(&list[0], day0(2026, 11, 12)), day0(2026, 11, 12)); /* today counts */
    CHECK_INT(own_next(&list[0], day0(2026, 11, 13)), day0(2027, 11, 12));
    CHECK_INT(own_next(&list[2], day0(2026, 10, 30)), 0);                  /* passed */
    CHECK_INT(own_next(&list[1], day0(2026, 10, 30)), day0(2026, 11, 3));
    CHECK_INT(own_next(&list[4], day0(2026, 10, 30)), day0(2030, 11, 20)); /* not before its year */

    /* Passed one-date events go 30 days after their day; those every year stay. */
    {
        own_event_t p[4] = {
            ev("Kari", OWN_BIRTHDAY, 1946, 1, 5, OWN_ON_ALL),
            ev("Tannlege", OWN_APPOINTMENT, 2026, 9, 30, OWN_ON_ALL),  /* ended 1 October */
            ev("Frisør", OWN_OTHER, 2026, 10, 1, OWN_ON_ALL),          /* ended 2 October */
            ev("Lege", OWN_APPOINTMENT, 2026, 12, 1, OWN_ON_ALL),
        };
        CHECK_INT(own_prune(p, 4, day0(2026, 10, 30), 30), 4);         /* none 30 days yet */
        CHECK_INT(own_prune(p, 4, day0(2026, 10, 31), 30), 3);         /* across summer time's end */
        CHECK_STR(p[0].text, "Kari");
        CHECK_STR(p[1].text, "Frisør");
        CHECK_STR(p[2].text, "Lege");
        CHECK_INT(own_prune(p, 3, day0(2028, 1, 1), 30), 1);
        CHECK_STR(p[0].text, "Kari");
    }

    /* Kept in NVS; what isn't valid is left out on loading. */
    test_nvs_clear();
    static own_event_t back[OWN_MAX];
    CHECK_INT(own_load(back), 0);
    list[2].kind = 9;
    CHECK(own_save(list, 5) == ESP_OK);
    CHECK_INT(own_load(back), 4);
    CHECK_STR(back[2].text, "Skuddår");
    CHECK(own_save(list, 0) == ESP_OK);
    CHECK_INT(own_load(back), 0);

    /* In the settings backup, and back. */
    list[2].kind = OWN_OTHER;
    static app_config_t cfg;
    app_config_load(&cfg);
    char *json = app_config_to_json(&cfg, false, list, 5);
    CHECK(json != NULL && strstr(json, "\"kind\":\t\"birthday\"") != NULL);
    char err[96] = "";
    int n = 0;
    memset(back, 0, sizeof(back));
    CHECK(app_config_from_json(json, &cfg, back, &n, err, sizeof(err)));
    CHECK_INT(n, 5);
    CHECK(memcmp(back, list, sizeof(list)) == 0);
    free(json);
    /* None: -1, the events there are kept; an empty list clears them. */
    json = app_config_to_json(&cfg, false, NULL, -1);
    CHECK(strstr(json, "\"events\"") == NULL);
    CHECK(app_config_from_json(json, &cfg, back, &n, err, sizeof(err)) && n == -1);
    free(json);
    json = app_config_to_json(&cfg, false, list, 0);
    CHECK(app_config_from_json(json, &cfg, back, &n, err, sizeof(err)) && n == 0);
    free(json);
    /* A bad event refuses the whole backup, the settings unchanged. */
    const uint8_t theme = cfg.theme;
    CHECK(!app_config_from_json("{\"format\":\"multidisplay-innstillinger\",\"version\":1,\"theme\":1,"
                                "\"events\":[{\"kind\":\"birthday\",\"text\":\"Kari\",\"day\":12,\"month\":11,"
                                "\"show\":{\"clock\":true}},{\"kind\":\"appointment\",\"text\":\"Uten år\",\"day\":1,"
                                "\"month\":2,\"show\":{\"clock\":true}}]}",
                                &cfg, back, &n, err, sizeof(err)));
    CHECK_STR(err, "Hendelse 2 i sikkerhetskopien er ikke gyldig.");
    CHECK_INT(cfg.theme, theme);
    CHECK(!app_config_from_json("{\"format\":\"multidisplay-innstillinger\",\"version\":1,\"events\":{}}",
                                &cfg, back, &n, err, sizeof(err)));
    CHECK(app_config_from_json("{\"format\":\"multidisplay-innstillinger\",\"version\":1,"
                               "\"events\":[{\"kind\":\"birthday\",\"text\":\"Kari\",\"day\":12,\"month\":11,"
                               "\"show\":{\"clock\":true}}]}",
                               &cfg, back, &n, err, sizeof(err)));
    CHECK(n == 1 && back[0].year == 0 && back[0].show == OWN_ON_CLOCK && back[0].kind == OWN_BIRTHDAY);

    /* Repeated: weekly on its weekday, monthly on its day (or the month's
     * last), from its date on. */
    own_event_t r[2] = {
        ev("Trening", OWN_APPOINTMENT, 2026, 11, 3, OWN_ON_ALL),  /* a Tuesday */
        ev("Husleie", OWN_OTHER, 2026, 10, 31, OWN_ON_ALL),
    };
    r[0].repeat = OWN_WEEKLY;
    r[1].repeat = OWN_MONTHLY;
    CHECK(!own_valid(&r[0]));                                      /* every 0 weeks */
    r[0].every = r[1].every = 1;
    CHECK(own_valid(&r[0]) && own_valid(&r[1]));
    own_event_t bad = ev("Kari", OWN_BIRTHDAY, 1946, 11, 12, OWN_ON_ALL);
    bad.repeat = OWN_WEEKLY;
    CHECK(!own_valid(&bad));                                       /* a birthday is every year already */
    bad = r[0];
    bad.repeat = OWN_REPEATS;
    CHECK(!own_valid(&bad));
    cal_list_t *ro = calloc(1, sizeof(cal_list_t) + 20 * sizeof(cal_event_t));
    ro->max = 20;
    CHECK_INT(own_days(r, 1, day0(2026, 11, 1), day0(2026, 12, 2), OWN_ON_ALL, ro), 5);
    CHECK_INT(ro->ev[0].start, day0(2026, 11, 3));
    CHECK_INT(ro->ev[3].start, day0(2026, 11, 24));
    CHECK_INT(ro->ev[4].start, day0(2026, 12, 1));
    ro->count = 0;
    CHECK_INT(own_days(r, 1, day0(2026, 10, 1), day0(2026, 11, 3), OWN_ON_ALL, ro), 0); /* not before its date */
    ro->count = 0;
    CHECK_INT(own_days(&r[1], 1, day0(2026, 10, 1), day0(2027, 3, 1), OWN_ON_ALL, ro), 5);
    CHECK_INT(ro->ev[0].start, day0(2026, 10, 31));
    CHECK_INT(ro->ev[1].start, day0(2026, 11, 30));            /* the 31st in a month of 30 */
    CHECK_INT(ro->ev[2].start, day0(2026, 12, 31));
    CHECK_INT(ro->ev[4].start, day0(2027, 2, 28));
    CHECK_STR(ro->ev[4].title, "Husleie");
    free(ro);
    CHECK_INT(own_next(&r[0], day0(2026, 10, 30)), day0(2026, 11, 3));
    CHECK_INT(own_next(&r[0], day0(2026, 11, 4)), day0(2026, 11, 10));
    CHECK_INT(own_next(&r[0], day0(2026, 11, 10)), day0(2026, 11, 10)); /* today counts */
    CHECK_INT(own_next(&r[1], day0(2026, 11, 1)), day0(2026, 11, 30));
    CHECK_INT(own_next(&r[1], day0(2026, 12, 31)), day0(2026, 12, 31));
    CHECK_INT(own_next(&r[1], day0(2027, 1, 1)), day0(2027, 1, 31));
    CHECK_INT(own_prune(r, 2, day0(2030, 1, 1), 30), 2);       /* never passed */

    /* Every n weeks or months. */
    own_event_t nr[2] = { r[0], r[1] };
    nr[0].every = 2;                                               /* every other Tuesday from 3 November */
    nr[1].every = 3;                                               /* every third month from 31 October */
    CHECK_INT(own_next(&nr[0], day0(2026, 11, 4)), day0(2026, 11, 17));
    CHECK_INT(own_next(&nr[0], day0(2026, 11, 17)), day0(2026, 11, 17));
    CHECK_INT(own_next(&nr[0], day0(2026, 11, 18)), day0(2026, 12, 1));
    CHECK_INT(own_next(&nr[1], day0(2026, 11, 1)), day0(2027, 1, 31));
    CHECK_INT(own_next(&nr[1], day0(2027, 2, 1)), day0(2027, 4, 30)); /* the 31st in April */
    CHECK_INT(own_next(&nr[1], day0(2027, 5, 1)), day0(2027, 7, 31));
    ro = calloc(1, sizeof(cal_list_t) + 20 * sizeof(cal_event_t));
    ro->max = 20;
    CHECK_INT(own_days(nr, 1, day0(2026, 11, 1), day0(2027, 1, 1), OWN_ON_ALL, ro), 5); /* 3/11 ... 29/12 */
    CHECK_INT(ro->ev[4].start, day0(2026, 12, 29));
    ro->count = 0;
    CHECK_INT(own_days(&nr[1], 1, day0(2026, 10, 1), day0(2027, 12, 31), OWN_ON_ALL, ro), 5);
    CHECK_INT(ro->ev[4].start, day0(2027, 10, 31));
    free(ro);
    nr[0].every = OWN_EVERY_WEEKS + 1;
    nr[1].every = OWN_EVERY_MONTHS + 1;
    CHECK(!own_valid(&nr[0]) && !own_valid(&nr[1]));
    nr[1].every = OWN_EVERY_MONTHS;
    CHECK(own_valid(&nr[1]));
    nr[1].repeat = OWN_ONCE;
    CHECK(!own_valid(&nr[1]));                                     /* once: every 0 */

    /* In the backup as "repeat"; none for once. */
    r[1].every = 3;
    cJSON *rj = own_to_json(r, 2);
    char *rs = cJSON_PrintUnformatted(rj);
    CHECK(strstr(rs, "\"repeat\":\"weekly\",\"show\"") != NULL);      /* every 1: left out */
    CHECK(strstr(rs, "\"repeat\":\"monthly\",\"every\":3") != NULL);
    own_event_t rb[OWN_MAX];
    CHECK_INT(own_from_json(rj, rb, err, sizeof(err)), 2);
    CHECK(memcmp(rb, r, sizeof(r)) == 0);
    cJSON_free(rs);
    cJSON_Delete(rj);
    rj = cJSON_Parse("[{\"kind\":\"appointment\",\"text\":\"x\",\"day\":1,\"month\":2,\"year\":2027,"
                     "\"repeat\":\"daily\",\"show\":{\"clock\":true}}]");
    CHECK_INT(own_from_json(rj, rb, err, sizeof(err)), -1);
    cJSON_Delete(rj);

    /* Saved before repeats (key v1): read as once, and moved to v2 by the
     * next save. */
    test_nvs_clear();
    own_event_v1_t old[2] = { { .text = "Kari", .year = 1946, .month = 11, .day = 12, .kind = OWN_BIRTHDAY,
                                .show = OWN_ON_ALL },
                              { .text = "Tannlege", .year = 2026, .month = 12, .day = 1, .kind = OWN_APPOINTMENT,
                                .show = OWN_ON_CLOCK } };
    nvs_handle_t h;
    CHECK(nvs_open(OWN_NVS_NS, NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_set_blob(h, OWN_NVS_V1, old, sizeof(old)) == ESP_OK);
    nvs_close(h);
    CHECK_INT(own_load(back), 2);
    CHECK_STR(back[1].text, "Tannlege");
    CHECK(back[1].repeat == OWN_ONCE && back[1].show == OWN_ON_CLOCK && back[0].year == 1946);
    CHECK(own_save(back, 2) == ESP_OK);
    size_t len = 0;
    CHECK(nvs_open(OWN_NVS_NS, NVS_READONLY, &h) == ESP_OK);
    CHECK(nvs_get_blob(h, OWN_NVS_V1, NULL, &len) == ESP_ERR_NVS_NOT_FOUND);
    nvs_close(h);
    CHECK_INT(own_load(back), 2);
}
