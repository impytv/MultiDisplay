/* Host tests: the parsers and helpers that need no hardware, built from the
 * firmware's own sources against stub ESP-IDF headers (stubs/). Run with
 * `make` in this folder; -v prints the firmware's log lines. */

#include <stdio.h>
#include <string.h>

#include "check.h"

int g_checks, g_failures;
extern int g_test_verbose;

void test_version(void);
void test_form(void);
void test_http(void);
void test_sun(void);
void test_entur(void);
void test_adsb(void);
void test_routes(void);
void test_owners(void);
void test_config(void);
void test_air(void);
void test_yr(void);
void test_tide(void);
void test_ical(void);
void test_sat(void);
void test_aurora(void);
void test_alerts(void);
void test_web_auth(void);
void test_holidays(void);
void test_astro(void);
void test_spotify(void);
void test_sonos(void);
void test_sgp4(void);
void test_png(void);
void test_own_events(void);

int main(int argc, char **argv)
{
    g_test_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "version", test_version }, { "form", test_form },   { "http", test_http },
        { "sun", test_sun },         { "entur", test_entur }, { "adsb", test_adsb },
        { "routes", test_routes },   { "owners", test_owners }, { "config", test_config }, { "air", test_air }, { "yr", test_yr },
        { "tide", test_tide }, { "ical", test_ical }, { "sat", test_sat }, { "aurora", test_aurora }, { "alerts", test_alerts },
        { "webauth", test_web_auth }, { "holidays", test_holidays }, { "astro", test_astro }, { "spotify", test_spotify },
        { "sonos", test_sonos }, { "sgp4", test_sgp4 }, { "png", test_png }, { "events", test_own_events },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        const int before = g_failures;
        tests[i].fn();
        printf("%-8s %s\n", tests[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
