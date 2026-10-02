#include <math.h>
#include <stdlib.h>
#include "check.h"
#include "../../components/tide_client.c"

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

void test_tide(void)
{
    static tide_t t;
    /* Kartverket's reply for Tromsø, hourly from 18:00 to midnight UTC: all
     * four series (observations up to 20:00). Read on a 20-minute grid. */
    char *xml = slurp("data/tide_all.xml");
    CHECK(xml != NULL);
    if (xml) {
        memset(&t, 0, sizeof(t));
        t.start = iso8601_to_epoch("2026-10-02T18:00:00Z");
        t.count = 19; /* 18:00 to 00:00 */
        CHECK(tide_parse_series(xml, &t));
        CHECK(!t.no_data);
        CHECK_STR(t.station, "Troms\xC3\xB8");
        CHECK(fabsf(t.prediction[0] - 217.9f) < 0.01f);
        CHECK(fabsf(t.forecast[3] - 177.1f) < 0.01f); /* 19:00 */
        CHECK(fabsf(t.observation[6] - 145.3f) < 0.01f); /* 20:00 */
        CHECK(isnan(t.observation[9]));                 /* 21:00: not measured yet */
        CHECK(isnan(t.prediction[1]));                  /* 18:20: hourly data only */
        CHECK(fabsf(t.prediction[18] - 115.5f) < 0.01f); /* midnight, the last point */
        /* Between points, and outside. */
        const int64_t t0 = t.start;
        CHECK(fabsf(tide_at(&t, t.forecast, t0 + 3 * 3600) - 108.5f) < 0.01f);
        CHECK(isnan(tide_at(&t, t.forecast, t0 + 3 * 3600 + 600))); /* 21:10: next to a gap */
        t.forecast[10] = 100.0f;
        CHECK(fabsf(tide_at(&t, t.forecast, t0 + 3 * 3600 + 600) - 104.25f) < 0.01f);
        CHECK(isnan(tide_at(&t, t.forecast, t0 - 1)));
        CHECK(fabsf(tide_at(&t, t.prediction, t0 + 6 * 3600) - 115.5f) < 0.01f);
        CHECK(isnan(tide_at(&t, t.prediction, t0 + 6 * 3600 + 1)));
        free(xml);
    }
    xml = slurp("data/tide_tab.xml");
    CHECK(xml != NULL);
    if (xml) {
        CHECK(tide_parse_extremes(xml, &t));
        CHECK_INT(t.extreme_count, 5);
        CHECK(!t.extremes[0].high && fabsf(t.extremes[0].cm - 88.6f) < 0.01f);
        CHECK_INT(t.extremes[0].time, iso8601_to_epoch("2026-10-02T22:27:00Z"));
        CHECK(t.extremes[1].high);
        free(xml);
    }
    /* Inland: a reply, but no data. Not a reply at all: false. */
    xml = slurp("data/tide_nodata.xml");
    CHECK(xml != NULL);
    if (xml) {
        CHECK(tide_parse_extremes(xml, &t));
        CHECK(t.no_data && t.extreme_count == 0 && t.station[0] == '\0');
        free(xml);
    }
    CHECK(!tide_parse_series("<html>502</html>", &t));
}
