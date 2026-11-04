/* When the ISS next passes over a place: the station's height over the
 * horizon, stepped through time from the elements (components/sgp4.c),
 * with long steps while it is far off. Checked against Skyfield in
 * test/host/test_iss_pass.c. */

#include <math.h>
#include <time.h>

#include "iss_pass.h"
#include "sun.h"

#define RAD (M_PI / 180.0)

#define NEAR_STEP_S 10.0   /* while it might rise: a pass above 10 degrees lasts 80 s or more */
#define PASS_STEP_S 5.0    /* through a pass, for the highest point and the part seen */
#define NEAR_DEG    16.0   /* from the place, beyond the ring it is 10 degrees up from (~12.5) */
#define FASTEST_DEG_S 0.075 /* how fast the point under it can come closer: 0.065 round, the earth 0.004 */
#define LONGEST_PASS_S 900.0
#define DARK_SUN_EL (-6.0)  /* the end of civil twilight */

typedef struct {
    double lat, lon;
    double obs[3], up[3]; /* the place on WGS-84 (km), straight up from it */
} place_t;

static void place_init(place_t *pl, double lat, double lon)
{
    const double a = 6378.137, e2 = 0.00669437999014;
    const double sl = sin(lat * RAD), cl = cos(lat * RAD), so = sin(lon * RAD), co = cos(lon * RAD);
    const double n = a / sqrt(1.0 - e2 * sl * sl);
    pl->lat = lat;
    pl->lon = lon;
    pl->obs[0] = n * cl * co;
    pl->obs[1] = n * cl * so;
    pl->obs[2] = n * (1.0 - e2) * sl;
    pl->up[0] = cl * co;
    pl->up[1] = cl * so;
    pl->up[2] = sl;
}

/* At `t`: its height over the place's horizon (degrees), and how far the
 * point under it is from the place (degrees round the earth); with lit,
 * whether it is in sunlight (outside the earth's shadow, taken as a
 * cylinder - near enough 400 km up). False if the elements give out. */
static bool look(const sgp4_t *s, const place_t *pl, double t, double *el, double *apart, bool *lit)
{
    double r[3], v[3];
    if (!sgp4_pos(s, t, r, v)) {
        return false;
    }
    if (lit != NULL) {
        double sun[3], slat, slon;
        sun_subsolar(t, &slat, &slon, sun);
        const double along = r[0] * sun[0] + r[1] * sun[1] + r[2] * sun[2];
        *lit = along > 0 || r[0] * r[0] + r[1] * r[1] + r[2] * r[2] - along * along >= 6371.0 * 6371.0;
    }
    /* Into the earth's frame. */
    const double g = sgp4_gmst(t), cg = cos(g), sg = sin(g);
    const double e[3] = { cg * r[0] + sg * r[1], -sg * r[0] + cg * r[1], r[2] };
    const double d[3] = { e[0] - pl->obs[0], e[1] - pl->obs[1], e[2] - pl->obs[2] };
    const double dl = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    *el = asin((d[0] * pl->up[0] + d[1] * pl->up[1] + d[2] * pl->up[2]) / dl) / RAD;
    const double el_len = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    const double ol = sqrt(pl->obs[0] * pl->obs[0] + pl->obs[1] * pl->obs[1] + pl->obs[2] * pl->obs[2]);
    double c = (e[0] * pl->obs[0] + e[1] * pl->obs[1] + e[2] * pl->obs[2]) / (el_len * ol);
    *apart = acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c) / RAD;
    return true;
}

/* Between a (below ISS_PASS_MIN_EL if `rising`, else above) and b, to a
 * second. */
static double crossing(const sgp4_t *s, const place_t *pl, double a, double b, bool rising)
{
    while (b - a > 1.0) {
        const double m = (a + b) / 2.0;
        double el, apart;
        if (!look(s, pl, m, &el, &apart, NULL)) {
            break;
        }
        if ((el >= ISS_PASS_MIN_EL) == rising) {
            b = m;
        } else {
            a = m;
        }
    }
    return (a + b) / 2.0;
}

/* The pass that rises between t_below and t_above, to its end. */
static bool follow(const sgp4_t *s, const place_t *pl, double t_below, double t_above, iss_pass_t *p)
{
    *p = (iss_pass_t){ .rise = crossing(s, pl, t_below, t_above, true) };
    p->top = p->rise;
    p->max_el = ISS_PASS_MIN_EL;
    for (double t = p->rise;; t += PASS_STEP_S) {
        double el, apart;
        bool lit;
        if (!look(s, pl, t, &el, &apart, &lit)) {
            return false;
        }
        if (el < ISS_PASS_MIN_EL && t > p->rise) {
            p->set = crossing(s, pl, t - PASS_STEP_S, t, false);
            break;
        }
        if (el > p->max_el) {
            p->max_el = el;
            p->top = t;
        }
        if (lit && sun_elevation_deg(pl->lat, pl->lon, (time_t)t) <= DARK_SUN_EL) {
            if (p->seen_from == 0) {
                p->seen_from = t;
                p->seen_max_el = el;
            }
            p->seen_to = t;
            if (el > p->seen_max_el) {
                p->seen_max_el = el;
            }
        }
        if (t - p->rise > LONGEST_PASS_S) {
            return false; /* not a pass, or not over the horizon at all */
        }
    }
    if (p->seen_from != 0 && p->seen_to < p->seen_from + PASS_STEP_S) {
        p->seen_to = p->seen_from + PASS_STEP_S; /* a glimpse of one step */
    }
    if (p->seen_to > p->set) {
        p->seen_to = p->set;
    }
    p->dark_at_top = sun_elevation_deg(pl->lat, pl->lon, (time_t)p->top) <= DARK_SUN_EL;
    return true;
}

bool iss_next_pass(const sgp4_t *s, double lat, double lon, double from, double until, bool seen_only,
                   iss_pass_t *p)
{
    place_t pl;
    place_init(&pl, lat, lon);
    /* From far enough back to find the rise of one under way. */
    double t = from - LONGEST_PASS_S, el, apart;
    if (!look(s, &pl, t, &el, &apart, NULL)) {
        return false;
    }
    while (t < until) {
        bool above = el >= ISS_PASS_MIN_EL;
        if (above) {
            /* Already up at the start: go back to below. */
            double back = t;
            while (above && t - back < LONGEST_PASS_S) {
                back -= NEAR_STEP_S;
                if (!look(s, &pl, back, &el, &apart, NULL)) {
                    return false;
                }
                above = el >= ISS_PASS_MIN_EL;
            }
            if (above) {
                return false;
            }
            t = back;
        }
        const double step = apart > NEAR_DEG ? fmax(NEAR_STEP_S, (apart - NEAR_DEG) / FASTEST_DEG_S) : NEAR_STEP_S;
        const double t2 = t + step;
        double el2, apart2;
        if (!look(s, &pl, t2, &el2, &apart2, NULL)) {
            return false;
        }
        if (el2 >= ISS_PASS_MIN_EL) {
            if (!follow(s, &pl, t, t2, p)) {
                return false;
            }
            if (p->set > from && (!seen_only || p->seen_from != 0)) {
                return p->rise < until;
            }
            t = p->set + PASS_STEP_S;
            if (!look(s, &pl, t, &el2, &apart2, NULL)) {
                return false;
            }
        } else {
            t = t2;
        }
        el = el2;
        apart = apart2;
    }
    return false;
}
