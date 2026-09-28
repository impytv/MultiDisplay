#pragma once

#include <stdbool.h>
#include <time.h>

/* The wall clock (see clock.c). */

/* Start SNTP: the router's NTP server if DHCP names one, then two public
 * pools. Call after the network stack is up and before WiFi connects, so
 * the DHCP answer can carry the router's server. */
void clock_start(void);

/* Whether the clock has been set (by NTP, or from an HTTP Date header). */
bool clock_is_set(void);

/* When and how it was last set: wall time (0 = never) and "NTP"/"HTTP". */
time_t clock_set_at(const char **source);
