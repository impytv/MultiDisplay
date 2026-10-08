#include <stdlib.h>
#include "check.h"
#include "../../components/met_alerts_client.c"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    b[fread(b, 1, n, f)] = '\0';
    fclose(f);
    return b;
}

void test_alerts(void)
{
    /* MET's reply for Kvaløysletta: one yellow snow warning for mountain
     * passes, in force from 12:00 UTC one day to 10:00 the next. */
    static met_alerts_t a;
    char *json = slurp("data/metalerts.json");
    CHECK(json != NULL);
    if (json) {
        CHECK(met_alerts_parse(json, &a));
        CHECK(a.valid);
        CHECK_INT(a.count, 1);
        CHECK_STR(a.alerts[0].event_name, "Sn\xC3\xB8");
        CHECK_STR(a.alerts[0].area, "Fjelloverganger i deler av Troms og Finnmark");
        CHECK_INT(a.alerts[0].color, MET_ALERT_YELLOW);
        CHECK_INT(a.alerts[0].start, iso8601_to_epoch("2026-10-08T12:00:00+00:00"));
        CHECK_INT(a.alerts[0].end, iso8601_to_epoch("2026-10-09T10:00:00+00:00"));
        free(json);
    }
    CHECK(met_alerts_parse("{\"features\":[]}", &a));
    CHECK_INT(a.count, 0);
    /* No "when": the alert is kept, with no times. */
    CHECK(met_alerts_parse("{\"features\":[{\"properties\":{\"riskMatrixColor\":\"Red\"}}]}", &a));
    CHECK_INT(a.count, 1);
    CHECK_INT(a.alerts[0].color, MET_ALERT_RED);
    CHECK_INT(a.alerts[0].start, 0);
    CHECK_INT(a.alerts[0].end, 0);
    CHECK(!met_alerts_parse("{\"type\":\"FeatureCollection\"}", &a));
    CHECK(!met_alerts_parse("not json", &a));
}
