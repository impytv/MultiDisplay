#include <math.h>
#include "check.h"
#include "iss_pass.h"

/* The ISS's elements from Celestrak, 2026-10-23 (as in test_sgp4.c). */
static const char *L1 = "1 25544U 98067A   26296.69304404  .00008515  00000+0  16237-3 0  9995";
static const char *L2 = "2 25544  51.6336  24.1765 0006871 300.7285  59.3026 15.49021515591674";

/* Over Oslo (59.91 N, 10.75 E) above 10 degrees in the three days after
 * the epoch, from Skyfield 1.55 (find_events, is_sunlit with DE421): rise,
 * top, set, the height at top, and whether a part of it can be seen. */
static const struct {
    double rise, top, set, max_el;
    bool seen;
} REF[] = {
    { 1792808007, 1792808050, 1792808093, 10.5, false }, /* 24.10 04:13, in the earth's shadow */
    { 1792813653, 1792813804, 1792813955, 19.3, true },
    { 1792819420, 1792819568, 1792819716, 18.7, true },  /* the sun 7 degrees down */
    { 1792897226, 1792897367, 1792897508, 17.6, true },  /* comes into the sunlight at the end */
    { 1792902976, 1792903130, 1792903284, 20.0, true },
    { 1792908802, 1792908886, 1792908971, 12.1, false }, /* the sun 2 degrees down */
    { 1792980805, 1792980930, 1792981055, 15.4, false },
    { 1792986535, 1792986691, 1792986848, 20.6, true },
    { 1792992334, 1792992451, 1792992569, 14.6, true },
};

void test_iss_pass(void)
{
    sgp4_t s;
    CHECK(sgp4_parse(L1, L2, &s));
    const double lat = 59.91, lon = 10.75, end = s.epoch + 3 * 86400.0;
    iss_pass_t p;
    double from = s.epoch;
    for (size_t i = 0; i < sizeof(REF) / sizeof(REF[0]); i++) {
        CHECK(iss_next_pass(&s, lat, lon, from, end, false, &p));
        CHECK(fabs(p.rise - REF[i].rise) < 3);
        CHECK(fabs(p.set - REF[i].set) < 3);
        CHECK(fabs(p.top - REF[i].top) < 10);
        CHECK(fabs(p.max_el - REF[i].max_el) < 0.2);
        CHECK((p.seen_from != 0) == REF[i].seen);
        if (p.seen_from != 0) {
            CHECK(p.seen_from >= p.rise && p.seen_to <= p.set && p.seen_from < p.seen_to);
            CHECK(p.seen_max_el >= ISS_PASS_MIN_EL && p.seen_max_el <= p.max_el + 0.01);
        }
        from = p.set + 1;
    }
    CHECK(!iss_next_pass(&s, lat, lon, from, end, false, &p));
    CHECK(iss_next_pass(&s, lat, lon, REF[5].rise - 60, end, false, &p));
    CHECK(!p.dark_at_top);
    CHECK(iss_next_pass(&s, lat, lon, REF[1].rise - 60, end, false, &p));
    CHECK(p.dark_at_top);

    /* One under way counts until it has set. */
    CHECK(iss_next_pass(&s, lat, lon, REF[1].top, end, false, &p));
    CHECK(fabs(p.rise - REF[1].rise) < 3);
    /* The next that can be seen skips those that can't. */
    CHECK(iss_next_pass(&s, lat, lon, REF[4].set + 1, end, true, &p));
    CHECK(fabs(p.rise - REF[7].rise) < 3);
    CHECK(iss_next_pass(&s, lat, lon, s.epoch, end, true, &p));
    CHECK(fabs(p.rise - REF[1].rise) < 3);
    /* Nothing that begins in the hour after the epoch. */
    CHECK(!iss_next_pass(&s, lat, lon, s.epoch, s.epoch + 3600, false, &p));
    /* Near the equator, where it passes high: still found, never above 90. */
    CHECK(iss_next_pass(&s, 0.0, 0.0, s.epoch, end, false, &p));
    CHECK(p.max_el > ISS_PASS_MIN_EL && p.max_el <= 90.0 && p.set > p.rise);
    /* Beyond its reach (it goes no further north than 52 degrees). */
    CHECK(!iss_next_pass(&s, 89.0, 0.0, s.epoch, end, false, &p));
}
