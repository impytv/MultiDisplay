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
        CHECK(fabsf(fc.days[8].temp_max_c - 12.5f) < 0.05f);
        CHECK_STR(fc.days[0].symbol_code, "partlycloudy_day");
        CHECK_STR(fc.days[1].symbol_code, "cloudy");
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
}
