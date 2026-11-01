#pragma once

/* Changes pushed by the Sonos group on show (UPnP GENA): its transport
 * (what plays, play/pause) and group volume. Each one wakes the weather
 * task to ask for what plays at once, so the screen follows the Sonos app
 * without asking every few seconds. If the speaker can't subscribe, the
 * screen polls as before. */

#include <stdbool.h>

#include "sonos_client.h"

/* Serve the speakers' NOTIFY. Once the web server is up. */
void sonos_events_add_handler(void);

/* Keep the subscriptions to `room` (a group's coordinator): made, renewed
 * before they run out, and moved when the room or an address changes. The
 * weather task, each poll. */
void sonos_events_keep(const sonos_room_t *room);

/* Whether both subscriptions are in place, so the polling can slow down. */
bool sonos_events_live(void);

/* End them (leaving the screen). The weather task. */
void sonos_events_stop(void);
