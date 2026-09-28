#pragma once

#include <stdbool.h>
#include <time.h>

/* Where the sun is, worked out on the device (no fetch): accurate to about
 * a minute for sunrise and sunset, which is all the weather screen needs. */

/* The sun's elevation in degrees above the horizon at `t`, seen from
 * lat/lon (degrees, east positive). Below SUN_DOWN_DEG it has set. */
double sun_elevation_deg(double lat, double lon, time_t t);

/* The upper rim on the horizon, with refraction: sunrise and sunset. */
#define SUN_DOWN_DEG (-0.833)

typedef enum { SUN_RISES_AND_SETS, SUN_UP_ALL_DAY, SUN_DOWN_ALL_DAY } sun_day_t;

/* The local day that `day` falls on: when the sun rises and sets (either
 * may be 0 if it only does one of them that day), or that it stays up or
 * down all day (midnight sun, polar night). */
sun_day_t sun_times(double lat, double lon, time_t day, time_t *rise, time_t *set);
