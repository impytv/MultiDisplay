/* PNG pictures as a square of RGB565 (see png_decode.h); no ESP-IDF, so the
 * host tests build it. */

#include <stdlib.h>
#include <string.h>

#include "png_decode.h"

#define PNG_SIDE_MAX 4096 /* wider or taller isn't a cover */

typedef struct {
    uint32_t w, h;
    int depth, ctype, chans;
    size_t rowbytes; /* a row's bytes, without its filter byte */
    int bpp;         /* bytes a pixel, at least 1 (as the filters count) */
    uint8_t pal[256][4];
    bool key;        /* tRNS of a grey or RGB picture: this colour is clear */
    uint16_t key_v[3];

    uint8_t *cur, *prev; /* rowbytes + 1 each: the row coming in (its filter
                            byte first), and the one before, unfiltered */
    size_t fill;         /* bytes of cur in so far */
    uint32_t y;          /* rows done */

    /* The square: s x s from ox, oy of the picture, into side x side. */
    uint16_t *out;
    int side;
    uint32_t s, ox, oy;
    uint8_t bg[3];
    uint16_t *xmap;   /* shrinking: each column's in the square; growing: each square column's in the row */
    uint32_t *acc;    /* shrinking: r, g, b and count for each column of row acc_ty */
    int acc_ty;
    int next_ty;      /* growing: the square's next row to fill */
} png_t;

bool png_is(const uint8_t *data, size_t len)
{
    static const uint8_t SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    return len >= sizeof(SIG) && memcmp(data, SIG, sizeof(SIG)) == 0;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Sample i of a row, at the picture's bit depth. */
static unsigned sample(const png_t *g, const uint8_t *row, size_t i)
{
    switch (g->depth) {
    case 8:
        return row[i];
    case 16:
        return (unsigned)row[2 * i] << 8 | row[2 * i + 1];
    default: {
        const size_t bit = i * (size_t)g->depth;
        return (row[bit / 8] >> (8 - g->depth - bit % 8)) & ((1u << g->depth) - 1);
    }
    }
}

/* A sample as 8 bits. */
static unsigned to8(const png_t *g, unsigned v)
{
    return g->depth == 16 ? v >> 8 : g->depth == 8 ? v : v * 255 / ((1u << g->depth) - 1);
}

/* Pixel x of an unfiltered row, over the background, into rgb. */
static void pixel(const png_t *g, const uint8_t *row, uint32_t x, unsigned rgb[3])
{
    unsigned a = 255;
    switch (g->ctype) {
    case 0: { /* grey */
        const unsigned v = sample(g, row, x);
        if (g->key && v == g->key_v[0]) {
            a = 0;
        }
        rgb[0] = rgb[1] = rgb[2] = to8(g, v);
        break;
    }
    case 2: { /* RGB */
        unsigned v[3];
        for (int c = 0; c < 3; c++) {
            v[c] = sample(g, row, 3 * (size_t)x + c);
            rgb[c] = to8(g, v[c]);
        }
        if (g->key && v[0] == g->key_v[0] && v[1] == g->key_v[1] && v[2] == g->key_v[2]) {
            a = 0;
        }
        break;
    }
    case 3: { /* palette */
        const uint8_t *p = g->pal[sample(g, row, x)];
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
        a = p[3];
        break;
    }
    case 4: /* grey and alpha */
        rgb[0] = rgb[1] = rgb[2] = to8(g, sample(g, row, 2 * (size_t)x));
        a = to8(g, sample(g, row, 2 * (size_t)x + 1));
        break;
    default: /* RGBA */
        for (int c = 0; c < 3; c++) {
            rgb[c] = to8(g, sample(g, row, 4 * (size_t)x + c));
        }
        a = to8(g, sample(g, row, 4 * (size_t)x + 3));
        break;
    }
    if (a < 255) {
        for (int c = 0; c < 3; c++) {
            rgb[c] = (rgb[c] * a + g->bg[c] * (255 - a) + 127) / 255;
        }
    }
}

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

/* Shrinking: the averages gathered for row acc_ty, into the square. */
static void flush_acc(png_t *g)
{
    if (g->acc_ty < 0) {
        return;
    }
    uint16_t *o = g->out + (size_t)g->acc_ty * g->side;
    for (int tx = 0; tx < g->side; tx++) {
        const uint32_t *a = &g->acc[4 * tx];
        const uint32_t n = a[3] ? a[3] : 1;
        o[tx] = rgb565((a[0] + n / 2) / n, (a[1] + n / 2) / n, (a[2] + n / 2) / n);
    }
    memset(g->acc, 0, (size_t)g->side * 4 * sizeof(g->acc[0]));
    g->acc_ty = -1;
}

/* Row y of the picture, unfiltered, into the square. */
static void put_row(png_t *g, const uint8_t *row, uint32_t y)
{
    if (y < g->oy || y >= g->oy + g->s) {
        return;
    }
    const uint32_t r = y - g->oy;
    unsigned rgb[3];
    if (g->s >= (uint32_t)g->side) {
        const int ty = (int)((uint64_t)r * g->side / g->s);
        if (ty != g->acc_ty) {
            flush_acc(g);
            g->acc_ty = ty;
        }
        for (uint32_t x = 0; x < g->s; x++) {
            pixel(g, row, g->ox + x, rgb);
            uint32_t *a = &g->acc[4 * g->xmap[x]];
            a[0] += rgb[0];
            a[1] += rgb[1];
            a[2] += rgb[2];
            a[3]++;
        }
        if (r == g->s - 1) {
            flush_acc(g);
        }
        return;
    }
    while (g->next_ty < g->side && (uint32_t)((uint64_t)g->next_ty * g->s / g->side) == r) {
        uint16_t *o = g->out + (size_t)g->next_ty * g->side;
        for (int tx = 0; tx < g->side; tx++) {
            pixel(g, row, g->ox + g->xmap[tx], rgb);
            o[tx] = rgb565(rgb[0], rgb[1], rgb[2]);
        }
        g->next_ty++;
    }
}

static int paeth(int a, int b, int c)
{
    const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* cur's row unfiltered in place, against prev's; false for an unknown
 * filter. */
static bool unfilter(png_t *g)
{
    uint8_t *x = g->cur + 1;
    const uint8_t *b = g->prev + 1;
    const size_t n = g->rowbytes, bpp = (size_t)g->bpp;
    switch (g->cur[0]) {
    case 0:
        break;
    case 1:
        for (size_t i = bpp; i < n; i++) {
            x[i] = (uint8_t)(x[i] + x[i - bpp]);
        }
        break;
    case 2:
        for (size_t i = 0; i < n; i++) {
            x[i] = (uint8_t)(x[i] + b[i]);
        }
        break;
    case 3:
        for (size_t i = 0; i < n; i++) {
            x[i] = (uint8_t)(x[i] + ((i >= bpp ? x[i - bpp] : 0) + b[i]) / 2);
        }
        break;
    case 4:
        for (size_t i = 0; i < n; i++) {
            x[i] = (uint8_t)(x[i] + paeth(i >= bpp ? x[i - bpp] : 0, b[i], i >= bpp ? b[i - bpp] : 0));
        }
        break;
    default:
        return false;
    }
    return true;
}

/* What the inflating gives: the rows, filled in turn. */
static bool take(void *ctx, const uint8_t *p, size_t n)
{
    png_t *g = ctx;
    while (n > 0 && g->y < g->h) {
        size_t k = g->rowbytes + 1 - g->fill;
        if (k > n) {
            k = n;
        }
        memcpy(g->cur + g->fill, p, k);
        g->fill += k;
        p += k;
        n -= k;
        if (g->fill == g->rowbytes + 1) {
            if (!unfilter(g)) {
                return false;
            }
            put_row(g, g->cur + 1, g->y);
            uint8_t *t = g->prev;
            g->prev = g->cur;
            g->cur = t;
            g->fill = 0;
            g->y++;
        }
    }
    return true; /* anything after the last row is left */
}

/* IHDR's fields; false for a picture this doesn't read. */
static bool header(png_t *g, const uint8_t *d, uint32_t len)
{
    if (len < 13) {
        return false;
    }
    g->w = be32(d);
    g->h = be32(d + 4);
    g->depth = d[8];
    g->ctype = d[9];
    static const int CHANS[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (g->w == 0 || g->h == 0 || g->w > PNG_SIDE_MAX || g->h > PNG_SIDE_MAX || g->ctype > 6 ||
        CHANS[g->ctype] == 0 || d[10] != 0 || d[11] != 0 || d[12] != 0 /* interlaced */) {
        return false;
    }
    const int depth = g->depth;
    const bool ok = g->ctype == 0   ? depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16
                    : g->ctype == 3 ? depth == 1 || depth == 2 || depth == 4 || depth == 8
                                    : depth == 8 || depth == 16;
    if (!ok) {
        return false;
    }
    g->chans = CHANS[g->ctype];
    g->rowbytes = ((size_t)g->w * g->chans * depth + 7) / 8;
    g->bpp = g->chans * depth / 8 > 0 ? g->chans * depth / 8 : 1;
    return true;
}

/* The buffers, once the size is known. */
static bool start(png_t *g)
{
    g->s = g->w < g->h ? g->w : g->h;
    g->ox = (g->w - g->s) / 2;
    g->oy = (g->h - g->s) / 2;
    g->cur = calloc(1, g->rowbytes + 1);
    g->prev = calloc(1, g->rowbytes + 1);
    const bool shrink = g->s >= (uint32_t)g->side;
    const size_t cols = shrink ? g->s : (size_t)g->side;
    g->xmap = malloc(cols * sizeof(g->xmap[0]));
    g->acc = shrink ? calloc((size_t)g->side * 4, sizeof(g->acc[0])) : NULL;
    if (g->cur == NULL || g->prev == NULL || g->xmap == NULL || (shrink && g->acc == NULL)) {
        return false;
    }
    for (size_t i = 0; i < cols; i++) {
        g->xmap[i] = (uint16_t)(shrink ? (uint64_t)i * g->side / g->s : (uint64_t)i * g->s / g->side);
    }
    return true;
}

bool png_decode_square(const uint8_t *png, size_t len, uint16_t *out, int side, uint32_t bg)
{
    if (!png_is(png, len) || side <= 0) {
        return false;
    }
    png_t *g = calloc(1, sizeof(*g));
    if (g == NULL) {
        return false;
    }
    g->out = out;
    g->side = side;
    g->acc_ty = -1;
    g->bg[0] = (uint8_t)(bg >> 16);
    g->bg[1] = (uint8_t)(bg >> 8);
    g->bg[2] = (uint8_t)bg;
    for (int i = 0; i < 256; i++) {
        g->pal[i][3] = 255;
    }
    png_zs_t *z = NULL;
    bool ok = false, have_header = false;
    int zs = 0;
    for (size_t at = 8; at + 12 <= len;) {
        const uint32_t clen = be32(png + at);
        const uint8_t *type = png + at + 4, *d = png + at + 8;
        if (clen > len - at - 12) {
            break; /* cut short */
        }
        at += 12 + (size_t)clen;
        if (memcmp(type, "IHDR", 4) == 0) {
            if (have_header || !header(g, d, clen) || !start(g)) {
                break;
            }
            have_header = true;
        } else if (!have_header) {
            break;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            for (uint32_t i = 0; i < clen / 3 && i < 256; i++) {
                memcpy(g->pal[i], d + 3 * i, 3);
            }
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (g->ctype == 3) {
                for (uint32_t i = 0; i < clen && i < 256; i++) {
                    g->pal[i][3] = d[i];
                }
            } else if ((g->ctype == 0 && clen >= 2) || (g->ctype == 2 && clen >= 6)) {
                g->key = true;
                for (int c = 0; c < (g->ctype == 0 ? 1 : 3); c++) {
                    g->key_v[c] = (uint16_t)(d[2 * c] << 8 | d[2 * c + 1]);
                }
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (z == NULL && (z = png_zs_open()) == NULL) {
                break;
            }
            if (zs == 0 && (zs = png_zs_feed(z, d, clen, take, g)) < 0) {
                break;
            }
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
    }
    ok = have_header && zs >= 0 && g->y == g->h;
    if (ok && g->s >= (uint32_t)g->side) {
        flush_acc(g); /* never left over, but to be sure */
    }
    png_zs_close(z);
    free(g->cur);
    free(g->prev);
    free(g->xmap);
    free(g->acc);
    free(g);
    return ok;
}
