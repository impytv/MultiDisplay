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
}
