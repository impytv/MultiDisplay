#include <math.h>
#include <stdlib.h>
#include "check.h"
#include "../../components/sat_util.c"

/* Shrink a sw x sh image of colour f(x, y) to dw x dh. */
static uint16_t *shrink(int sw, int sh, int dw, int dh, uint8_t (*f)(int x, int y))
{
    uint16_t *dst = calloc((size_t)dw * dh, sizeof(uint16_t));
    uint8_t *row = malloc((size_t)sw * 3);
    sat_shrink_t s;
    CHECK(sat_shrink_init(&s, sw, sh, dw, dh, dst));
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            row[3 * x] = row[3 * x + 1] = row[3 * x + 2] = f(x, y);
        }
        sat_shrink_row(&s, y, row);
    }
    sat_shrink_free(&s);
    free(row);
    return dst;
}

static uint8_t grey(int x, int y) { return 200; }
static uint8_t stripes(int x, int y) { return (x & 1) ? 255 : 0; }
static uint8_t halves(int x, int y) { return y < 360 ? 0 : 248; }

/* The 8 bits of red back from RGB565, as the top five. */
static int red(uint16_t px) { return (px >> 8) & 0xF8; }

void test_sat(void)
{
    /* Where places land: as fitted (scripts/fit_satellite.py), and the
     * coastline there checked by eye on the image. */
    float x, y;
    sat_project(60.0571, 10.8613, &x, &y); /* Nittedal */
    CHECK(fabsf(x - 783.93f) < 0.05f && fabsf(y - 228.59f) < 0.05f);
    sat_project(69.6857, 18.8080, &x, &y); /* Kvaløysletta */
    CHECK(fabsf(x - 796.69f) < 0.05f && fabsf(y - 87.22f) < 0.05f);
    sat_project(36.14, -5.35, &x, &y); /* Gibraltar */
    CHECK(fabsf(x - 641.16f) < 0.05f && fabsf(y - 586.92f) < 0.05f);

    /* A close-up's corner: centred, or pushed inside near the top. */
    int x0, y0;
    CHECK(sat_crop(60.0571, 10.8613, 267, 150, &x0, &y0));
    CHECK_INT(x0, 650);
    CHECK_INT(y0, 154);
    CHECK(sat_crop(71.0, 25.0, 267, 150, &x0, &y0)); /* Nordkapp, by the top edge */
    CHECK_INT(y0, 0);
    CHECK(!sat_crop(-33.9, 18.4, 267, 150, &x0, &y0)); /* Cape Town */
    CHECK(!sat_crop(35.7, 139.7, 267, 150, &x0, &y0)); /* Tokyo */

    CHECK_INT(sat_image_time("inline;filename=\"Europe-IR-20261006044500.png\""), 1791261900);
    CHECK_INT(sat_image_time("inline;filename=\"Europe-IR.png\""), 0);
    CHECK_INT(sat_image_time("inline"), 0);

    /* Shrinking: an even colour stays it; 1280 -> 800 averages fine
     * stripes to grey; a sharp edge on a result row's boundary stays sharp
     * (720 -> 450: row 225 starts at source row 360). */
    uint16_t *d = shrink(SAT_IMG_W, SAT_IMG_H, 800, 450, grey);
    CHECK(d[0] == sat_rgb565(200, 200, 200) && d[800 * 450 - 1] == sat_rgb565(200, 200, 200));
    free(d);
    d = shrink(SAT_IMG_W, SAT_IMG_H, 800, 450, stripes);
    int lo = 255, hi = 0;
    for (int i = 0; i < 800; i++) {
        lo = red(d[800 * 100 + i]) < lo ? red(d[800 * 100 + i]) : lo;
        hi = red(d[800 * 100 + i]) > hi ? red(d[800 * 100 + i]) : hi;
    }
    CHECK(lo >= 96 && hi <= 160); /* 1.6 px per pixel: 2/5..3/5 white */
    free(d);
    d = shrink(SAT_IMG_W, SAT_IMG_H, 800, 450, halves);
    CHECK_INT(red(d[800 * 224 + 5]), 0);
    CHECK_INT(red(d[800 * 225 + 5]), 248);
    free(d);
    /* An exact halving: 4 x 2 -> 2 x 1 averages pairs. */
    d = shrink(4, 2, 2, 1, stripes);
    CHECK_INT(red(d[0]), 128 & 0xF8);
    CHECK_INT(red(d[1]), 128 & 0xF8);
    free(d);
}
