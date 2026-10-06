#ifndef _SAT_UTIL_H_
#define _SAT_UTIL_H_

/* The satellite screen's sums that need no hardware: where a place is in
 * MET Norway's satellite image of Europe, the part of it shown around a
 * place, and shrinking the image to fit the screen. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* The image of Europe (geosatellite/1.4, area=europe), px. */
#define SAT_IMG_W 1280
#define SAT_IMG_H 720

/**
 * Where (lat, lon) is in the image of Europe, in pixels (may be outside
 * it). The image is a spherical Lambert conformal conic projection; fitted
 * by scripts/fit_satellite.py, as the API doesn't document it.
 */
void sat_project(double lat, double lon, float *x, float *y);

/**
 * The top-left corner of the `w` x `h` px part of the image centred on
 * (lat, lon), moved inside the image where the place is near its edge.
 * false if the place isn't in the image at all.
 */
bool sat_crop(double lat, double lon, int w, int h, int *x0, int *y0);

/**
 * When the image was taken, from its Content-Disposition header:
 * inline;filename="Europe-IR-20261006044500.png" (UTC). 0 if not there.
 */
time_t sat_image_time(const char *content_disposition);

/* Shrinks an image fed in row by row (RGB888, top to bottom) by averaging
 * the area of the source behind each pixel, into palette indices: each
 * result pixel, as RGB565, looked up in `lut` (see sat_palette.bin). */
typedef struct {
    int sw, sh;     /* source size */
    int dw, dh;     /* result size, at most the source size each way */
    uint8_t *dst;   /* dw x dh indices, filled as rows complete */
    const uint8_t *lut; /* 65536: the palette index for each RGB565 value */
    uint32_t *hrow; /* dw x 3: the current source row, shrunk across */
    uint32_t *acc;  /* 2 x dw x 3: the two result rows it can touch */
} sat_shrink_t;

/* Set up to shrink `sw` x `sh` into `dst` (`dw` x `dh`) through `lut`; the
 * work rows are allocated (in PSRAM on the device). false if out of memory. */
bool sat_shrink_init(sat_shrink_t *s, int sw, int sh, int dw, int dh, uint8_t *dst, const uint8_t *lut);
void sat_shrink_row(sat_shrink_t *s, int y, const uint8_t *rgb);
void sat_shrink_free(sat_shrink_t *s);

/* RGB565 as LVGL's LV_COLOR_FORMAT_RGB565 holds it. */
static inline uint16_t sat_rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#endif
