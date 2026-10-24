#include <math.h>
#include "check.h"
#include "sgp4.h"

/* The ISS's elements from Celestrak, 2026-10-23. */
static const char *L1 = "1 25544U 98067A   26296.69304404  .00008515  00000+0  16237-3 0  9995";
static const char *L2 = "2 25544  51.6336  24.1765 0006871 300.7285  59.3026 15.49021515591674";

/* From python-sgp4 2.27 (Vallado's code, WGS-72), minutes after the epoch. */
static const struct {
    double min, r[3], v[3];
} REF[] = {
    { 0, { 6199.986314, 2783.327047, -0.001490 }, { -1.948993396, 4.336390210, 6.009890762 } },
    { 30, { -4295.195218, 2223.723632, 4774.477872 }, { -5.409668692, -4.721704660, -2.660186708 } },
    { 95, { 5911.125695, 3266.801555, 754.503789 }, { -2.897777096, 3.864538919, 5.949196391 } },
    { 720, { 1569.605821, -3918.884459, -5328.844373 }, { 7.108150713, 2.837760584, 0.010390692 } },
    { 4320, { -6700.655835, -1163.923203, -79.012348 }, { 0.879247920, -4.669169616, -6.004446828 } },
};

/* Append the checksum to a TLE line of 68 characters. */
static void with_checksum(char *line)
{
    int sum = 0;
    for (int i = 0; i < 68; i++) {
        sum += (line[i] >= '0' && line[i] <= '9') ? line[i] - '0' : line[i] == '-';
    }
    line[68] = (char)('0' + sum % 10);
    line[69] = '\0';
}

void test_sgp4(void)
{
    sgp4_t s;
    CHECK(sgp4_parse(L1, L2, &s));
    CHECK_INT(s.catnr, 25544);
    CHECK(fabs(s.epoch - 1792773479.005056) < 0.01);
    CHECK(fabs(s.bstar - 0.16237e-3) < 1e-12);
    for (size_t i = 0; i < sizeof(REF) / sizeof(REF[0]); i++) {
        double r[3], v[3];
        CHECK(sgp4_pos(&s, s.epoch + REF[i].min * 60.0, r, v));
        for (int k = 0; k < 3; k++) {
            CHECK(fabs(r[k] - REF[i].r[k]) < 0.001);  /* to the metre */
            CHECK(fabs(v[k] - REF[i].v[k]) < 1e-6);
        }
    }

    /* GMST at J2000.0 (2000-01-01 12:00 UTC): 280.46062 degrees. */
    CHECK(fabs(sgp4_gmst(946728000.0) * 180.0 / M_PI - 280.46062) < 0.0001);

    /* At the epoch it crosses the equator northwards; over three days it
     * stays within its inclination, at its height and speed. */
    sgp4_ground_t g;
    CHECK(sgp4_ground(&s, s.epoch, &g));
    CHECK(fabs(g.lat) < 0.01);
    double max_lat = 0;
    for (double t = 0; t < 3 * 86400; t += 300) {
        CHECK(sgp4_ground(&s, s.epoch + t, &g));
        max_lat = fmax(max_lat, fabs(g.lat));
        CHECK(g.lon >= -180 && g.lon <= 180);
        CHECK(g.alt_km > 400 && g.alt_km < 440);
        CHECK(g.speed_kmh > 27400 && g.speed_kmh < 27800);
    }
    CHECK(max_lat > 51.6 && max_lat < 52.0); /* geodetic: a little over 51.63 */

    /* As Celestrak sends it. */
    char text[256];
    snprintf(text, sizeof(text), "ISS (ZARYA)             \r\n%s\r\n%s\r\n", L1, L2);
    sgp4_t t;
    CHECK(sgp4_parse_text(text, &t));
    CHECK(t.epoch == s.epoch && t.no_kozai == s.no_kozai);
    CHECK(!sgp4_parse_text("No GP data found", &t));
    /* As ARISS sends it: \n only, its own way of writing a zero. */
    CHECK(sgp4_parse_text("ISS (ZARYA)\n"
                          "1 25544U 98067A   26296.69304404  .00008515  00000-0  16237-3 0  9996\n"
                          "2 25544  51.6336  24.1765 0006871 300.7285  59.3026 15.49021515591674\n\n",
                          &t));
    CHECK(t.epoch == s.epoch && t.no_kozai == s.no_kozai);
    snprintf(text, sizeof(text), "%s\n%s\n", L2, L1);
    CHECK(!sgp4_parse_text(text, &t));

    /* A changed digit breaks the checksum; deep space isn't handled. */
    char bad[80];
    snprintf(bad, sizeof(bad), "%s", L2);
    bad[10] = '7';
    CHECK(!sgp4_parse(L1, bad, &s));
    CHECK(!sgp4_parse(L1, "2 25544", &s));
    char geo1[80], geo2[80];
    snprintf(geo1, sizeof(geo1), "%s", "1 28626U 05008A   26296.50000000 -.00000123  00000+0  00000+0 0  999");
    snprintf(geo2, sizeof(geo2), "%s", "2 28626   0.0153 251.8573 0002331 189.1285 124.7183  1.00271005 7917");
    with_checksum(geo1);
    with_checksum(geo2);
    CHECK(!sgp4_parse(geo1, geo2, &s));
    geo2[52] = '1'; /* 11 revolutions a day: near-earth, so it does */
    geo2[68] = '\0';
    with_checksum(geo2);
    CHECK(sgp4_parse(geo1, geo2, &s));
}
