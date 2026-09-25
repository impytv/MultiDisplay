#ifndef _RAIN_CLIENT_H_
#define _RAIN_CLIENT_H_

#include <stdint.h>
#include <time.h>
#include "esp_err.h"

/* Rain intensity levels, as the five colours of MET Norway's
 * "5level_reflectivity" radar images: roughly 0.03, 0.1, 1, 2.5 and 5 mm/h
 * and up. 0 is dry (or not covered). */
#define RAIN_LEVELS 5

/* One of MET's radar image areas and where its pixels lie: a spherical
 * Lambert conformal conic projection (standard parallel lat1, central
 * meridian lon0) and the image grid on it. Fitted from the images by
 * scripts/fit_radar_areas.py - the API doesn't document them. */
typedef struct {
    const char *name;  /* the API's area= value */
    uint16_t w, h;     /* image size, px */
    float lat1, lon0;  /* degrees */
    float m_per_px;
    float ox, oy;      /* pixel = (x, -y) / m_per_px - (ox, oy), x/y from the cone apex */
} rain_area_t;

typedef struct {
    const rain_area_t *area;
    uint8_t *level; /* area->w x area->h, one of 0..RAIN_LEVELS per pixel */
    time_t time;    /* when the radar image was taken, 0 if unknown */
} rain_image_t;

/**
 * The radar area that best covers `range_km` around (lat, lon): the finest
 * one holding the whole circle, else the one covering most of it. NULL if
 * the point is on none of them.
 */
const rain_area_t *rain_client_pick_area(double lat, double lon, float range_km);

/**
 * Where (lat, lon) is in `area`'s image, in pixels (may be outside it).
 */
void rain_client_project(const rain_area_t *area, double lat, double lon, float *px, float *py);

/**
 * Fetch the latest radar image of `area` from api.met.no and turn it into
 * rain levels in `img` (img->level is allocated in PSRAM and reused across
 * calls; the previous content is kept on failure).
 */
esp_err_t rain_client_fetch(const rain_area_t *area, rain_image_t *img);

#endif
