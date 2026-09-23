#ifndef _AIS_CLIENT_H_
#define _AIS_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Ships kept per fetch: the nearest ones win when there are more in range. */
#define AIS_MAX_SHIPS 32

typedef enum {
    AIS_CAT_OTHER = 0,
    AIS_CAT_CARGO,
    AIS_CAT_TANKER,
    AIS_CAT_PASSENGER,
    AIS_CAT_FISHING,
    AIS_CAT_LEISURE,
    AIS_CAT_TUG,
    AIS_CAT_COUNT,
} ais_category_t;

typedef struct {
    char name[21];        /* AIS name, trimmed; the MMSI when it has none */
    uint32_t mmsi;
    float dist_km;        /* from the query centre */
    float bearing_deg;    /* from the query centre, clockwise from north */
    float sog_kn;         /* speed over ground, 0 if unknown */
    float cog_deg;        /* course over ground, clockwise from north */
    float heading_deg;    /* true heading; the course when not reported */
    float age_s;          /* age of the position report at fetch time */
    uint8_t category;     /* ais_category_t */
    bool moored;          /* moored / at anchor / barely moving */
} ais_ship_t;

typedef struct {
    int count;            /* entries in ship[], sorted nearest first */
    int total;            /* ships with a position inside the circle that pass the length filter */
    ais_ship_t ship[AIS_MAX_SHIPS];
} ais_result_t;

/**
 * Fetch the ships within `radius_km` of (lat, lon) from the BarentsWatch
 * Live AIS API, authenticating with the OAuth client credentials. Ships
 * shorter than `min_len_m` metres are left out - and, when it is above 0, so
 * are ships that don't report a length. The access token is cached and
 * renewed shortly before it expires. On success `out`
 * holds the nearest AIS_MAX_SHIPS, nearest first.
 *
 * Returns ESP_ERR_INVALID_ARG when no credentials are configured and
 * ESP_ERR_INVALID_STATE when BarentsWatch rejects them; ESP_FAIL otherwise.
 */
esp_err_t ais_client_fetch(const char *client_id, const char *client_secret,
                           double lat, double lon, float radius_km, uint16_t min_len_m,
                           ais_result_t *out);

/** Drop the kept-alive connection (frees its TLS buffers). Safe to call any time. */
void ais_client_close(void);

/** Short Norwegian label for a category, for the ship table. */
const char *ais_category_label(uint8_t category);

#endif
