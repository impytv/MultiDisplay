#ifndef _AIR_CLIENT_H_
#define _AIR_CLIENT_H_

/* Air quality and pollen for a location, hour by hour:
 *
 *  - air quality from MET Norway's airqualityforecast (all of Norway, about
 *    two days ahead): the overall index and one per pollutant, on MET's
 *    scale where 1-2 is little pollution, 2-3 moderate, 3-4 high and 4+
 *    very high;
 *  - pollen (alder, birch, grass, mugwort) from Open-Meteo's air-quality
 *    API, which serves the European Copernicus (CAMS) model, in grains per
 *    cubic metre. A model forecast, not NAAF's official pollen forecast. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "http_util.h"

#define AIR_HOURS 48

typedef enum { AIR_PM25, AIR_PM10, AIR_NO2, AIR_O3, AIR_POLLUTANTS } air_pollutant_t;

typedef struct {
    bool valid;
    int count;              /* hours in aqi[] */
    int64_t start;          /* epoch seconds of the first hour */
    float aqi[AIR_HOURS];   /* the overall index */
    float sub[AIR_POLLUTANTS][AIR_HOURS];
} air_quality_t;

typedef enum { AIR_ALDER, AIR_BIRCH, AIR_GRASS, AIR_MUGWORT, AIR_POLLEN_TYPES } air_pollen_type_t;

typedef struct {
    bool valid;
    int count;
    int64_t start;
    float grains[AIR_POLLEN_TYPES][AIR_HOURS]; /* per m3; 0 when not given */
} air_pollen_t;

/* Fetch the air quality forecast (HTTP_NOT_MODIFIED with `cache`, as for
 * the weather, when the one held is current; `out` is then untouched). */
esp_err_t air_quality_fetch(double lat, double lon, air_quality_t *out, http_cache_t *cache);

/* Fetch the pollen forecast for today and tomorrow. */
esp_err_t air_pollen_fetch(double lat, double lon, air_pollen_t *out);

/* The parsers, for the host tests. */
bool air_quality_parse(const char *json, size_t len, air_quality_t *out);
bool air_pollen_parse(const char *json, air_pollen_t *out);

/* The hour of `start`-based data holding `now`, or -1. */
int air_hour_index(int64_t start, int count, int64_t now);

/* MET's level for an index: 0 little, 1 moderate, 2 high, 3 very high. */
int air_aqi_level(float aqi);
const char *air_aqi_level_name(int level);   /* "Lite", "Moderat", "H\xC3\xB8y", "Sv\xC3\xA6rt h\xC3\xB8y" */
uint32_t air_aqi_level_colour(int level);    /* 0xRRGGBB, MET's colours */
const char *air_pollutant_name(air_pollutant_t p);

/* NAAF's pollen levels: 0 none, 1 slight, 2 moderate, 3 heavy, 4 extreme. */
int air_pollen_level(air_pollen_type_t type, float grains);
const char *air_pollen_level_name(int level); /* "Ingen", "Beskjeden", ... */
uint32_t air_pollen_level_colour(int level);
const char *air_pollen_name(air_pollen_type_t type); /* "Or", "Bj\xC3\xB8rk", "Gress", "Burot" */

#endif
