#ifndef _ENTUR_CLIENT_H_
#define _ENTUR_CLIENT_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Public transport departures from Entur's Journey Planner v3; its
 * estimatedCalls already carry the SIRI realtime data.
 *
 * Which stops and lines to show is one line of text (app_config_t.departures):
 * stops separated by ';', each a stop place ID, optionally followed by '='
 * and a comma-separated list of line IDs. A line ID may end in "/in" or
 * "/out" to keep only that direction, or - for lines without one, such as
 * Vy's trains - in "/v<stop>[+<stop>...]" to keep the departures that call
 * at one of those stop places (NSR:StopPlace:<stop>) later on. A bare number
 * is short for NSR:StopPlace:<number>, and a stop without lines shows every
 * line, e.g.
 *
 *     58366=RUT:Line:31/out,RUT:Line:25;6505;58858=VYG:Line:R31/v502
 *
 * The setup page's departure picker (setup_departures.js) writes it. */

#define ENTUR_MAX_STOPS      4
#define ENTUR_MAX_LINES      8   /* per stop */
#define ENTUR_ID_MAX         40
#define ENTUR_MAX_GROUPS     24  /* line + direction rows over all stops */
#define ENTUR_PER_GROUP      2   /* departures kept per row */

#define ENTUR_MAX_VIA        3   /* stops naming a "/v" direction */

#define ENTUR_DIR_IN  0x01
#define ENTUR_DIR_OUT 0x02

typedef struct {
    char id[ENTUR_ID_MAX];
    uint8_t dirs;      /* ENTUR_DIR_* mask; 0 = both */
    uint8_t via_count; /* > 0: only departures calling at one of via[] later */
    uint32_t via[ENTUR_MAX_VIA]; /* NSR:StopPlace numbers */
} entur_line_sel_t;

typedef struct {
    char stop_id[ENTUR_ID_MAX];
    int line_count; /* 0 = every line */
    entur_line_sel_t lines[ENTUR_MAX_LINES];
} entur_stop_sel_t;

typedef struct {
    int stop_count;
    entur_stop_sel_t stops[ENTUR_MAX_STOPS];
} entur_selection_t;

typedef struct {
    int64_t expected; /* epoch seconds */
    int64_t aimed;
    bool realtime;
    bool cancelled;
} entur_call_t;

typedef struct {
    uint8_t stop;          /* index into entur_departures_t.stop_name */
    char code[8];          /* line publicCode, e.g. "31" */
    char mode[12];         /* transportMode, e.g. "bus" */
    char dest[40];         /* destinationDisplay.frontText of the first call */
    char quay[8];          /* quay publicCode of the first call ("" if none) */
    char notice[160];      /* a disruption notice for the row's departures, e.g.
                            * "Færre vogner: Denne avgangen kjører ..." ("" = none) */
    bool has_colour;
    uint32_t colour;       /* line colour, 0xRRGGBB */
    uint32_t text_colour;
    int call_count;
    entur_call_t calls[ENTUR_PER_GROUP];
} entur_group_t;

typedef struct {
    bool valid;
    int stop_count;
    char stop_name[ENTUR_MAX_STOPS][48];
    int group_count;       /* sorted by stop, line, then first departure */
    entur_group_t groups[ENTUR_MAX_GROUPS];
} entur_departures_t;

/**
 * Parse the selection text described above. Returns false (with a reason in
 * the log) if it holds no usable stop; entries beyond the limits are dropped.
 */
bool entur_parse_selection(const char *text, entur_selection_t *out);

/**
 * Fetch the next departures for every stop in `sel`, grouped per line and
 * direction, into `out`.
 */
esp_err_t entur_client_fetch(const entur_selection_t *sel, entur_departures_t *out);

/** Drop the kept-alive HTTPS connection (and its TLS buffers). */
void entur_client_close(void);

#endif
