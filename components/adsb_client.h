#ifndef _ADSB_CLIENT_H_
#define _ADSB_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Aircraft kept per fetch: the nearest ones win when there are more in range. */
#define ADSB_MAX_AIRCRAFT 32

/* alt_ft value meaning "not reported". */
#define ADSB_ALT_UNKNOWN INT32_MIN

typedef struct {
    char callsign[9];     /* flight, trimmed; falls back to the registration, then the ICAO hex */
    char type[5];         /* ICAO type designator, e.g. "B38M"; "" if unknown */
    int32_t alt_ft;       /* barometric altitude, or ADSB_ALT_UNKNOWN */
    float dist_km;        /* from the query centre */
    float bearing_deg;    /* from the query centre, clockwise from north */
    float gs_kt;          /* ground speed, 0 if unknown */
    float track_deg;      /* ground track, clockwise from north */
    float seen_pos_s;     /* age of the position report at fetch time */
} adsb_aircraft_t;

typedef struct {
    int count;            /* entries in ac[], sorted nearest first */
    int total;            /* airborne aircraft with a position that the server returned */
    adsb_aircraft_t ac[ADSB_MAX_AIRCRAFT];
} adsb_result_t;

/**
 * Fetch the airborne aircraft within `radius_km` of (lat, lon) from the open
 * adsb.fi ADS-B API (https://opendata.adsb.fi/). Aircraft on the ground and
 * ones without a position are dropped. On success `out` holds the nearest
 * ADSB_MAX_AIRCRAFT, nearest first. (The API itself works in nautical
 * miles; distances are converted to kilometres here.)
 *
 * The HTTPS connection is kept open between calls (keep-alive) so a poll
 * every few seconds doesn't pay a TLS handshake - and its internal-DRAM
 * spike - each time. Call adsb_client_close() when polling stops.
 *
 * The API allows one request per second; callers must space calls further
 * apart than that.
 */
esp_err_t adsb_client_fetch(double lat, double lon, float radius_km, adsb_result_t *out);

/** Drop the kept-alive connection (frees its TLS buffers). Safe to call any time. */
void adsb_client_close(void);

#endif
