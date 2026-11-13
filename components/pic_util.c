#include "pic_util.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool ext_is(const char *ext, const char *want)
{
    for (; *want; ext++, want++) {
        if (toupper((unsigned char)*ext) != *want) {
            return false;
        }
    }
    return *ext == '\0';
}

bool pic_is_picture(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || name[0] == '.') {
        return false; /* "._PHOTO.JPG" and the like are a Mac's, not pictures */
    }
    return ext_is(dot + 1, "JPG") || ext_is(dot + 1, "JPE") || ext_is(dot + 1, "JPEG");
}

long pic_number(const char *name)
{
    if (toupper((unsigned char)name[0]) != 'B') {
        return -1;
    }
    long n = 0;
    for (int i = 1; i <= 7; i++) {
        if (!isdigit((unsigned char)name[i])) {
            return -1;
        }
        n = n * 10 + (name[i] - '0');
    }
    if (name[8] != '.' || !ext_is(name + 9, "JPG") || n == 0) {
        return -1;
    }
    return n;
}

void pic_name(char *buf, long n, const char *ext)
{
    unsigned long v = n < 0 ? 0 : (unsigned long)(n % 10000000);
    buf[0] = 'B';
    for (int i = 7; i >= 1; i--, v /= 10) {
        buf[i] = (char)('0' + v % 10);
    }
    buf[8] = '.';
    snprintf(buf + 9, PIC_NAME_MAX - 9, "%.3s", ext);
}

bool pic_fit(int w, int h, int max_w, int max_h, int *out_w, int *out_h)
{
    if (w <= 0 || h <= 0 || max_w < 8 || max_h < 8) {
        return false;
    }
    if (w <= max_w && h <= max_h) {
        *out_w = w;
        *out_h = h;
        return true;
    }
    /* The side that limits it first, then the other in proportion; both
     * down to a multiple of 8. */
    int tw, th;
    if ((long long)w * max_h >= (long long)h * max_w) {
        tw = max_w;
        th = (int)((long long)h * max_w / w);
    } else {
        th = max_h;
        tw = (int)((long long)w * max_h / h);
    }
    tw -= tw % 8;
    th -= th % 8;
    if (tw < 8 || th < 8 || tw * 8 < w || th * 8 < h) {
        return false;
    }
    *out_w = tw;
    *out_h = th;
    return true;
}

void pic_bag_reset(pic_bag_t *b, int n)
{
    b->n = n < 0 ? 0 : (n > b->cap ? b->cap : n);
    b->pos = b->n; /* shuffled on the first pick */
    b->last = -1;
}

int pic_bag_next(pic_bag_t *b, uint32_t (*rnd)(void))
{
    if (b->n <= 0) {
        return -1;
    }
    if (b->pos >= b->n) {
        for (int i = 0; i < b->n; i++) {
            b->order[i] = (uint16_t)i;
        }
        for (int i = b->n - 1; i > 0; i--) {
            const int j = (int)(rnd() % (uint32_t)(i + 1));
            const uint16_t t = b->order[i];
            b->order[i] = b->order[j];
            b->order[j] = t;
        }
        /* Not the one just shown first again. */
        if (b->n > 1 && b->order[0] == b->last) {
            const int j = 1 + (int)(rnd() % (uint32_t)(b->n - 1));
            b->order[0] = b->order[j];
            b->order[j] = (uint16_t)b->last;
        }
        b->pos = 0;
    }
    b->last = b->order[b->pos++];
    return b->last;
}

/* What starts like one of the pictures a browser reads. BMP and the
 * ISO-media formats (HEIC, AVIF) are checked past their first bytes: "BM"
 * alone, or "ftyp" at 4 - which every MP4 and MOV has - would let other
 * files through. */
bool pic_looks_like_picture(const uint8_t *b, size_t n)
{
    static const char *const brands[] = { "heic", "heix", "heim", "heis", "hevc", "hevx",
                                          "mif1", "msf1", "avif", "avis" };
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) { /* JPEG */
        return true;
    }
    if ((n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) ||
        (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0)) ||
        (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)) {
        return true;
    }
    if (n >= 18 && b[0] == 'B' && b[1] == 'M') {
        /* Reserved bytes 6..9 zero, and a known size of the header after
         * the 14-byte file header. */
        const uint32_t dib = (uint32_t)b[14] | (uint32_t)b[15] << 8 | (uint32_t)b[16] << 16 | (uint32_t)b[17] << 24;
        return b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0 &&
               (dib == 12 || dib == 40 || dib == 52 || dib == 56 || dib == 64 || dib == 108 || dib == 124);
    }
    if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0) {
        /* The major brand, at 8, says which kind of ISO media it is. */
        for (size_t i = 0; i < sizeof(brands) / sizeof(brands[0]); i++) {
            if (memcmp(b + 8, brands[i], 4) == 0) {
                return true;
            }
        }
    }
    return false;
}
