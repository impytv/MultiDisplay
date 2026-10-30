#pragma once

/* The display's own events (components/own_events.h): held in memory for the
 * screens, and the page /hendelser where they are typed in. */

#include <stdint.h>

#include "ical.h"

/* Load them from NVS. Before the screens are built. */
void events_init(void);

/* Serve /hendelser. Once the web server is up. */
void events_add_handlers(void);

/* Changes each time the events are saved, for the screens to redraw. Any
 * task. */
uint32_t events_generation(void);

/* How many there are. Any task. */
int events_count(void);

/* Append the days of the events shown `where` (OWN_ON_*) that meet [from,
 * to) to `out`, as own_days. Any task. */
int events_days(int64_t from, int64_t to, uint8_t where, cal_list_t *out);
