#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "civil_time.h"
#include "sat_util.h"

/* Fitted by scripts/fit_satellite.py against the coastline drawn on the
 * image (within about a pixel over Norway); run it again if MET changes the
 * image (its size is checked on every fetch). The standard parallel and
 * central meridian (degrees), pixels per Earth radius, and the pixel where
 * the two cross. */
#define SAT_LAT1 69.86989
#define SAT_LON0 -0.26216
#define SAT_K    795.08387
#define SAT_CX   706.30258
#define SAT_CY   98.90146

void sat_project(double lat, double lon, float *x, float *y)
{
    const double d2r = M_PI / 180.0;
    const double p1 = SAT_LAT1 * d2r;
    const double n = sin(p1);
    const double f = cos(p1) * pow(tan(M_PI / 4 + p1 / 2), n) / n;
    const double rho = f / pow(tan(M_PI / 4 + lat * d2r / 2), n);
    const double rho0 = f / pow(tan(M_PI / 4 + p1 / 2), n);
    const double th = n * (lon - SAT_LON0) * d2r;
    *x = (float)(SAT_CX + SAT_K * rho * sin(th));
    *y = (float)(SAT_CY - SAT_K * (rho0 - rho * cos(th)));
}

bool sat_crop(double lat, double lon, int w, int h, int *x0, int *y0)
{
    float x, y;
    sat_project(lat, lon, &x, &y);
    if (!(x >= 0 && x < SAT_IMG_W && y >= 0 && y < SAT_IMG_H)) {
        return false;
    }
    int cx = (int)lroundf(x - w / 2.0f), cy = (int)lroundf(y - h / 2.0f);
    *x0 = cx < 0 ? 0 : (cx > SAT_IMG_W - w ? SAT_IMG_W - w : cx);
    *y0 = cy < 0 ? 0 : (cy > SAT_IMG_H - h ? SAT_IMG_H - h : cy);
    return true;
}

time_t sat_image_time(const char *v)
{
    const char *d = strrchr(v, '-');
    int y, mo, day, h, mi, s;
    if (d == NULL || sscanf(d + 1, "%4d%2d%2d%2d%2d%2d", &y, &mo, &day, &h, &mi, &s) != 6 || mo < 1 || mo > 12 ||
        day < 1 || day > 31 || h > 23 || mi > 59 || s > 60) {
        return 0;
    }
    return (time_t)(days_from_civil(y, mo, day) * 86400 + h * 3600 + mi * 60 + s);
}

/* Across and down alike: source pixel x covers [x * dn, (x + 1) * dn) and
 * result pixel i covers [i * sn, (i + 1) * sn), on a common scale; a source
 * pixel meets at most two result pixels, as dn <= sn. The overlaps are the
 * weights, summing to sn for each result pixel. */

bool sat_shrink_init(sat_shrink_t *s, int sw, int sh, int dw, int dh, uint16_t *dst)
{
    *s = (sat_shrink_t){ .sw = sw, .sh = sh, .dw = dw, .dh = dh, .dst = dst };
    s->hrow = heap_caps_malloc((size_t)dw * 3 * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s->acc = heap_caps_calloc((size_t)dw * 6, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (s->hrow == NULL || s->acc == NULL || dw > sw || dh > sh || dw < 1 || dh < 1) {
        sat_shrink_free(s);
        return false;
    }
    return true;
}

void sat_shrink_free(sat_shrink_t *s)
{
    heap_caps_free(s->hrow);
    heap_caps_free(s->acc);
    s->hrow = s->acc = NULL;
}

void sat_shrink_row(sat_shrink_t *s, int y, const uint8_t *rgb)
{
    const int sw = s->sw, dw = s->dw, sh = s->sh, dh = s->dh;
    uint32_t *const h = s->hrow;
    memset(h, 0, (size_t)dw * 3 * sizeof(uint32_t));
    for (int x = 0; x < sw; x++) {
        const int a = x * dw, i = a / sw;
        const int w0 = ((i + 1) * sw < a + dw ? (i + 1) * sw : a + dw) - a;
        const uint8_t *p = rgb + 3 * x;
        uint32_t *o = h + 3 * i;
        o[0] += (uint32_t)w0 * p[0];
        o[1] += (uint32_t)w0 * p[1];
        o[2] += (uint32_t)w0 * p[2];
        if (w0 < dw) {
            const uint32_t w1 = (uint32_t)(dw - w0);
            o[3] += w1 * p[0];
            o[4] += w1 * p[1];
            o[5] += w1 * p[2];
        }
    }

    const int a = y * dh, j = a / sh;
    const int w0 = ((j + 1) * sh < a + dh ? (j + 1) * sh : a + dh) - a;
    uint32_t *const r0 = s->acc + (size_t)(j & 1) * dw * 3;
    uint32_t *const r1 = s->acc + (size_t)((j + 1) & 1) * dw * 3;
    for (int k = 0; k < dw * 3; k++) {
        r0[k] += (uint32_t)w0 * h[k];
        if (w0 < dh) {
            r1[k] += (uint32_t)(dh - w0) * h[k];
        }
    }
    if (a + dh >= (j + 1) * sh && j < dh) {
        /* Row j has all of its source rows: out it goes. */
        const uint32_t total = (uint32_t)sw * (uint32_t)sh, half = total / 2;
        uint16_t *out = s->dst + (size_t)j * dw;
        for (int i = 0; i < dw; i++) {
            out[i] = sat_rgb565((r0[3 * i] + half) / total, (r0[3 * i + 1] + half) / total,
                                (r0[3 * i + 2] + half) / total);
        }
        memset(r0, 0, (size_t)dw * 3 * sizeof(uint32_t));
    }
}
