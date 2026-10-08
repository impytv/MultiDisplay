#ifndef _MET_ALERTS_CLIENT_H_
#define _MET_ALERTS_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "http_util.h"

/* A point on the map is essentially never covered by more than a couple of
 * simultaneously active warnings. */
#define MET_ALERTS_MAX 4

/* MET Norway's three warning colours, in increasing severity - the numeric
 * value doubles as an ordering so the worst active alert can be picked with
 * a plain max(). */
typedef enum {
    MET_ALERT_YELLOW = 1,
    MET_ALERT_ORANGE = 2,
    MET_ALERT_RED = 3,
} met_alert_color_t;

typedef struct {
    char event_name[80]; /* eventAwarenessName: human-readable Norwegian text,
                           * e.g. "Faretruende bygevind" */
    char area[96];        /* the named area the alert covers, e.g. "Fjelloverganger i
                           * deler av Troms og Finnmark" */
    met_alert_color_t color;
    int64_t start, end;   /* when it's in force (epoch UTC; 0 if not given) */
} met_alert_t;

typedef struct {
    bool valid;
    int count; /* number of entries in alerts[] (<= MET_ALERTS_MAX); 0 is a
                * valid, successful result - it just means nothing is active
                * there right now. */
    met_alert_t alerts[MET_ALERTS_MAX];
} met_alerts_t;

/**
 * Fetch the MET Norway severe weather alerts ("farevarsel") whose area
 * covers (lat, lon), from the MetAlerts API: those in force now and those
 * issued for later (see start/end). The API does the geometry; alerts that
 * have ended are left out by it, but one held on the device can end before
 * the next fetch.
 */
/* With `cache` (see http_util.h), HTTP_NOT_MODIFIED if the alerts held for
 * that location are still current; `out` is then untouched. */
/* Parse a MetAlerts current.json reply into `out` (cleared first); false if
 * it isn't one. */
bool met_alerts_parse(const char *json, met_alerts_t *out);

esp_err_t met_alerts_client_fetch(double lat, double lon, met_alerts_t *out, http_cache_t *cache);

#endif
