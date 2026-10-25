#include <math.h>
#include <stdlib.h>
#include "check.h"
#include "../../components/yr_client.c"

static char *slurp_file(const char *path)
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

static int64_t local_midnight(int y, int m, int d)
{
    struct tm tm = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_isdst = -1 };
    return mktime(&tm);
}

void test_yr(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    /* A real forecast for Nittedal; the expected days were worked out
     * separately (Python, same rules). */
    static yr_forecast_t fc;
    char *json = slurp_file("data/forecast.json");
    CHECK(json != NULL);
    if (json) {
        CHECK(parse_forecast(json, &fc));
        free(json);
        CHECK_INT(fc.point_count, YR_FORECAST_BASE_POINTS);
        CHECK_INT(fc.day_count, YR_DAYS);
        CHECK_INT(fc.days[0].start, local_midnight(2026, 9, 29));
        CHECK_INT(fc.days[3].start, local_midnight(2026, 10, 2));
        CHECK(fabsf(fc.days[0].temp_max_c - 16.0f) < 0.05f && fabsf(fc.days[0].temp_min_c - 5.4f) < 0.05f);
        CHECK(fabsf(fc.days[3].precip_mm - 3.6f) < 0.05f);
        CHECK(fabsf(fc.days[3].temp_min_c - 10.1f) < 0.05f);
        /* The high end of the range, and gusts while MET gives them. */
        CHECK(fabsf(fc.days[3].precip_max_mm - 9.5f) < 0.05f);
        CHECK(fabsf(fc.days[1].precip_max_mm - 0.7f) < 0.05f && fc.days[1].precip_mm < 0.05f);
        CHECK(fabsf(fc.days[1].gust_max_ms - 7.0f) < 0.05f);
        CHECK(fc.days[3].gust_max_ms == 0.0f);
        CHECK(fabsf(fc.days[8].temp_max_c - 12.5f) < 0.05f);
        CHECK_STR(fc.days[0].symbol_code, "partlycloudy_day");
        CHECK_STR(fc.days[1].symbol_code, "cloudy");
    }

    /* Numbers the forecast doesn't read are left out before parsing; the
     * rest, strings, arrays and escaped quotes stay as they were. */
    char d1[] = "{\"a\":1,\"wind_speed\":-2.5e1,\"b\":{\"c\":3},\"d\":[4,5],\"e\":\"x\\\":6\",\"f\":7}";
    drop_unused_numbers(d1);
    CHECK_STR(d1, "{\"wind_speed\":-2.5e1,\"b\":{},\"d\":[4,5],\"e\":\"x\\\":6\"}");
    char d2[] = "{\"air_temperature\":1,\"x\":2}";
    drop_unused_numbers(d2);
    CHECK_STR(d2, "{\"air_temperature\":1}");
    char d3[] = "{\"x\":2,\"wind_speed\":1}";
    drop_unused_numbers(d3);
    CHECK_STR(d3, "{\"wind_speed\":1}");
    char d4[] = "{\"x\" :2,\"y\":\"cut";
    drop_unused_numbers(d4);
    CHECK_STR(d4, "{\"x\" :2,\"y\":\"cut");
    json = slurp_file("data/forecast.json");
    if (json) {
        const size_t before = strlen(json);
        drop_unused_numbers(json);
        cJSON *root = cJSON_Parse(json);
        CHECK(root != NULL && strlen(json) < before * 3 / 4);
        cJSON_Delete(root);
        free(json);
    }

    /* When precipitation starts and stops. */
    static yr_nowcast_t nc;
    memset(&nc, 0, sizeof(nc));
    nc.valid = nc.radar_ok = true;
    nc.point_count = 18;
    for (int i = 0; i < nc.point_count; i++) {
        nc.points[i].epoch_utc = 1000000 + i * 300;
    }
    int64_t at;
    CHECK(yr_rain_change(&nc, 1000100, &at) == YR_RAIN_NONE);
    nc.points[6].precipitation_rate = 0.4f;                  /* from step 6 */
    CHECK(yr_rain_change(&nc, 1000100, &at) == YR_RAIN_STARTS);
    CHECK_INT(at, 1000000 + 6 * 300);
    for (int i = 0; i < nc.point_count; i++) {
        nc.points[i].precipitation_rate = i < 4 ? 1.2f : 0.0f;
    }
    CHECK(yr_rain_change(&nc, 1000100, &at) == YR_RAIN_STOPS);
    CHECK_INT(at, 1000000 + 4 * 300);
    CHECK(yr_rain_change(&nc, 1000000 + 5 * 300, &at) == YR_RAIN_NONE); /* after it stopped */
    for (int i = 0; i < nc.point_count; i++) {
        nc.points[i].precipitation_rate = 2.0f;
    }
    CHECK(yr_rain_change(&nc, 1000100, &at) == YR_RAIN_ONGOING);
    nc.radar_ok = false;
    CHECK(yr_rain_change(&nc, 1000100, &at) == YR_RAIN_NONE);   /* no radar: say nothing */
    nc.points[0].precipitation_rate = 0.05f;                    /* below the threshold is dry */
    nc.radar_ok = true;
    for (int i = 1; i < nc.point_count; i++) {
        nc.points[i].precipitation_rate = 0.05f;
    }
    CHECK(yr_rain_change(&nc, 1000100, &at) == YR_RAIN_NONE);

    /* The radar down (as for southern Norway on 2026-10-18): one step, no
     * rate, but the temperature now - which the clock screen shows. */
    static const char *const no_radar =
        "{\"properties\":{\"meta\":{\"updated_at\":\"2026-10-18T11:27:00Z\","
        "\"radar_coverage\":\"temporarily unavailable\"},\"timeseries\":[{\"time\":\"2026-10-18T11:25:00Z\","
        "\"data\":{\"instant\":{\"details\":{\"air_temperature\":5.4,\"relative_humidity\":62.1,"
        "\"wind_from_direction\":86.2,\"wind_speed\":0.6,\"wind_speed_of_gust\":2.6}},"
        "\"next_1_hours\":{\"summary\":{\"symbol_code\":\"partlycloudy_day\"}}}}]}}";
    memset(&nc, 0, sizeof(nc));
    CHECK(parse_nowcast(no_radar, &nc));
    CHECK(!nc.radar_ok);
    CHECK_INT(nc.point_count, 1);
    CHECK(nc.points[0].has_instant_details && fabsf(nc.points[0].air_temperature_c - 5.4f) < 0.05f);
    CHECK(yr_rain_change(&nc, 1792322700, &at) == YR_RAIN_NONE);

    /* With the radar up, a step without a rate is still skipped, and a
     * response with nothing usable is refused. */
    static const char *const radar_ok =
        "{\"properties\":{\"meta\":{\"radar_coverage\":\"ok\"},\"timeseries\":["
        "{\"time\":\"2026-10-18T11:25:00Z\",\"data\":{\"instant\":{\"details\":{\"air_temperature\":5.6}}}},"
        "{\"time\":\"2026-10-18T11:30:00Z\",\"data\":{\"instant\":{\"details\":{\"precipitation_rate\":0.3}}}}]}}";
    memset(&nc, 0, sizeof(nc));
    CHECK(parse_nowcast(radar_ok, &nc));
    CHECK_INT(nc.point_count, 1);
    CHECK(!nc.points[0].has_instant_details && fabsf(nc.points[0].precipitation_rate - 0.3f) < 0.01f);
    static const char *const nothing =
        "{\"properties\":{\"meta\":{\"radar_coverage\":\"temporarily unavailable\"},\"timeseries\":["
        "{\"time\":\"2026-10-18T11:25:00Z\",\"data\":{\"instant\":{\"details\":{\"wind_speed\":1}}}}]}}";
    memset(&nc, 0, sizeof(nc));
    CHECK(!parse_nowcast(nothing, &nc));
}
