#ifndef _SGP4_H_
#define _SGP4_H_

/* Where a satellite is, from its two-line elements (TLE): the near-earth
 * part of SGP4 (Hoots and Roehrich, Spacetrack Report #3, as revised by
 * Vallado et al. 2006), with the WGS-72 constants the elements are made
 * for. Orbits of 225 minutes or more (deep space) aren't handled - the
 * ISS goes round in about 93. Pure C, no ESP-IDF, so it is host-tested. */

#include <stdbool.h>
#include <time.h>

typedef struct {
    double epoch;  /* epoch seconds (UTC) of the elements */
    double bstar;  /* drag term, 1 / earth radii */
    double inclo, nodeo, ecco, argpo, mo; /* radians, and the eccentricity */
    double no_kozai; /* mean motion, radians per minute */
    int catnr;
    /* Set up by sgp4_init. */
    double no_unkozai, ao, con41, con42, cc1, cc4, cc5, d2, d3, d4, delmo, eta, argpdot, omgcof, sinmao,
        t2cof, t3cof, t4cof, t5cof, x1mth2, x7thm1, mdot, nodedot, xlcof, xmcof, nodecf, aycof;
    bool isimp;
} sgp4_t;

/* Parse a TLE's two lines into `s` and set it up for sgp4_pos; false if
 * they aren't a TLE (checksums included) or the orbit is a deep-space one. */
bool sgp4_parse(const char *line1, const char *line2, sgp4_t *s);

/* The same from a text with the two lines in it among others (Celestrak's
 * answer has the name on a line before them), each ended by \n or \r\n. */
bool sgp4_parse_text(const char *text, sgp4_t *s);

/* The position (km) and velocity (km/s) at epoch seconds `t`, in the TEME
 * frame; false if the elements give nonsense there (decayed). */
bool sgp4_pos(const sgp4_t *s, double t, double r[3], double v[3]);

/* Greenwich mean sidereal time at epoch seconds `t`, radians. */
double sgp4_gmst(double t);

/* The point under it at `t`: latitude and longitude (degrees, geodetic on
 * WGS-84, east positive, -180..180), height above the ground (km), and
 * speed (km/h, relative to the stars - as the ISS's is usually quoted). */
typedef struct {
    double lat, lon, alt_km, speed_kmh;
} sgp4_ground_t;

bool sgp4_ground(const sgp4_t *s, double t, sgp4_ground_t *g);

#endif
