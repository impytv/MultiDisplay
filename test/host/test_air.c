#include <math.h>
#include <stdlib.h>
#include "check.h"
#include "../../components/air_client.c"

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

void test_air(void)
{
    static air_quality_t q;
    char *json = slurp("data/airquality.json"); /* MET's reply, cut to five hours */
    CHECK(json != NULL);
    if (json) {
        CHECK(air_quality_parse(json, strlen(json), &q));
        CHECK_INT(q.count, 5);
        CHECK_INT(q.start, iso8601_to_epoch("2026-09-28T18:00:00Z"));
        CHECK(fabsf(q.aqi[0] - 1.815f) < 0.001f);
        CHECK(fabsf(q.sub[AIR_PM25][0] - 1.266f) < 0.001f);
        CHECK(q.sub[AIR_O3][0] == q.aqi[0]); /* ozone was the highest */
        free(json);
    }
    /* A gap in the hours ends the series there. */
    static const char gap[] = "{\"data\":{\"time\":["
        "{\"from\":\"2026-09-28T18:00:00Z\",\"variables\":{\"AQI\":{\"value\":1.5}}},"
        "{\"from\":\"2026-09-28T19:00:00Z\",\"variables\":{\"AQI\":{\"value\":2.5},\"AQI_no2\":{\"units\":\"1\",\"value\":2.5}}},"
        "{\"from\":\"2026-09-28T21:00:00Z\",\"variables\":{\"AQI\":{\"value\":3.5}}}]}}";
    CHECK(air_quality_parse(gap, strlen(gap), &q));
    CHECK_INT(q.count, 2);
    CHECK(q.sub[AIR_NO2][1] == 2.5f && q.sub[AIR_PM10][1] == 0);
    CHECK(!air_quality_parse("{\"data\":{}}", 11, &q));
    CHECK(!air_quality_parse("x", 1, &q));

    static air_pollen_t p;
    json = slurp("data/pollen.json"); /* Open-Meteo's reply: 48 hours, all zero in late September */
    CHECK(json != NULL);
    if (json) {
        CHECK(air_pollen_parse(json, &p));
        CHECK_INT(p.count, 48);
        CHECK_INT(p.start, 1790553600);
        CHECK(p.grains[AIR_BIRCH][10] == 0);
        free(json);
    }
    CHECK(air_pollen_parse("{\"hourly\":{\"time\":[100,3700],\"birch_pollen\":[12.5,null]}}", &p));
    CHECK(p.grains[AIR_BIRCH][0] == 12.5f && p.grains[AIR_BIRCH][1] == 0 && p.grains[AIR_GRASS][0] == 0);

    CHECK_INT(air_hour_index(1000, 5, 999), -1);
    CHECK_INT(air_hour_index(1000, 5, 1000), 0);
    CHECK_INT(air_hour_index(1000, 5, 1000 + 3600 * 4 + 3599), 4);
    CHECK_INT(air_hour_index(1000, 5, 1000 + 3600 * 5), -1);

    CHECK_INT(air_aqi_level(1.9f), 0);
    CHECK_INT(air_aqi_level(2.0f), 1);
    CHECK_INT(air_aqi_level(3.99f), 2);
    CHECK_INT(air_aqi_level(7), 3);
    CHECK_INT(air_pollen_level(AIR_BIRCH, 0.4f), 0);
    CHECK_INT(air_pollen_level(AIR_BIRCH, 50), 2);
    CHECK_INT(air_pollen_level(AIR_BIRCH, 5000), 4);
    CHECK_INT(air_pollen_level(AIR_GRASS, 50), 3);
    CHECK_INT(air_pollen_level(AIR_GRASS, 150), 4);
    CHECK_STR(air_pollen_level_name(1), "Beskjeden");
}
