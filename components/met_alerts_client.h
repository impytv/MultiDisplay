#ifndef _MET_ALERTS_CLIENT_H_
#define _MET_ALERTS_CLIENT_H_

#include <stdbool.h>
#include "esp_err.h"

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
    char area[48];        /* the named area the alert covers, e.g. "Oslo" */
    met_alert_color_t color;
} met_alert_t;

typedef struct {
    bool valid;
    int count; /* number of entries in alerts[] (<= MET_ALERTS_MAX); 0 is a
                * valid, successful result - it just means nothing is active
                * there right now. */
    met_alert_t alerts[MET_ALERTS_MAX];
} met_alerts_t;

/**
 * Fetch the currently active MET Norway severe weather alerts ("farevarsel")
 * covering (lat, lon), from the MetAlerts API. The API itself filters to
 * alerts whose area covers the point and that are active right now, so no
 * date or geometry filtering is needed on the device.
 */
esp_err_t met_alerts_client_fetch(double lat, double lon, met_alerts_t *out);

#endif
