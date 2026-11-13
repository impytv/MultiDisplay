#pragma once

/* Pure helpers for the picture screen (main/pictures.c): which files on the
 * memory card are pictures, what the display names its own, the size a
 * picture is decoded at, and the shuffled order they are shown in. No
 * ESP-IDF, so they are host-tested (test/host/test_pics.c). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A picture's file name on the card, in UTF-8 with its '\0': long names
 * up to 63 bytes ("Sommer på hytta 2024.jpg"); longer ones are left out.
 * The display's own are B0000001.JPG and up. */
#define PIC_NAME_MAX 64

/* Whether `name` (as the card lists it) is a JPEG picture to show: .jpg,
 * .jpeg or .jpe, any case; not a Mac's "._" files or other hidden ones. */
bool pic_is_picture(const char *name);

/* The number in one of the display's own names ("B0000042.JPG" -> 42), or
 * -1 for any other name. */
long pic_number(const char *name);

/* The name for picture `n` (1..9999999), into buf (PIC_NAME_MAX), with
 * extension `ext` ("JPG", or "TMP" while it is being written). */
void pic_name(char *buf, long n, const char *ext);

/* The size to decode a w x h picture at to fit in max_w x max_h: its own
 * size if it fits, else scaled down keeping its shape, both sides a
 * multiple of 8 (the decoder's rule for scaling) and at least 1/8 of the
 * original (the most it scales by). False if it can't be: too large to
 * fit even at 1/8, or no size at all. */
bool pic_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h);

/* The order pictures are shown in: each once, at random, before any comes
 * again, and never the same twice in a row across a reshuffle (unless
 * there is only one). `order` has room for `cap` indices. */
typedef struct {
    uint16_t *order;
    int cap;
    int n;    /* pictures it is for */
    int pos;  /* next in order */
    int last; /* last shown, -1 = none */
} pic_bag_t;

/* Start over for `n` pictures (at most cap); nothing shown yet. */
void pic_bag_reset(pic_bag_t *b, int n);

/* The next picture's index (0..n-1), or -1 with none. rnd() gives random
 * 32-bit numbers (esp_random on the display). */
int pic_bag_next(pic_bag_t *b, uint32_t (*rnd)(void));

/* Whether the first n bytes of a file (at least 18 for a BMP, 12 for the
 * rest) start like a picture a browser reads: JPEG, PNG, GIF, WebP, BMP,
 * HEIC or AVIF - and not, say, an MP4 film, a web page or a text file. */
bool pic_looks_like_picture(const uint8_t *b, size_t n);
