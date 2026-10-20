#ifndef _PNG_DECODE_H_
#define _PNG_DECODE_H_

/* PNG pictures (covers and logos that aren't JPEG, e.g. Spotify's "Liked
 * Songs") as a square of RGB565, decoded as a stream: only two rows of the
 * picture are held, whatever its size, and the square is built as the rows
 * come. Plain C, tested on the host; the inflating is the platform's (see
 * png_zs_*). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether `data` starts as a PNG does. */
bool png_is(const uint8_t *data, size_t len);

/* The picture's middle square at `side` x `side` px, as RGB565 into out:
 * shrunk by averaging, grown by the nearest pixel, transparency drawn over
 * `bg` (0xRRGGBB). Every colour type and bit depth; not interlaced ones.
 * False if it can't be read (out is then partly written). */
bool png_decode_square(const uint8_t *png, size_t len, uint16_t *out, int side, uint32_t bg);

/* ---- The inflating, per platform: png_inflate.c on the display (the ROM's
 * tinfl), zlib in the host tests ---- */

typedef struct png_zs png_zs_t;

/* A zlib stream to inflate; NULL without memory. */
png_zs_t *png_zs_open(void);

/* The next `len` bytes of it; what comes out is handed to out() (false from
 * it stops). 1 once the stream has ended, 0 for more, -1 if it's broken. */
int png_zs_feed(png_zs_t *z, const uint8_t *in, size_t len, bool (*out)(void *ctx, const uint8_t *p, size_t n),
                void *ctx);

void png_zs_close(png_zs_t *z);

#endif
