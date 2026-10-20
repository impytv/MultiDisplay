/* PNG's zlib streams inflated in the host tests (see png_decode.h), with
 * zlib in place of the display's ROM tinfl. */

#include <stdlib.h>
#include <zlib.h>

#include "png_decode.h"

struct png_zs {
    z_stream s;
    bool done;
    uint8_t buf[4096]; /* smaller than tinfl's window, so rows span pieces */
};

png_zs_t *png_zs_open(void)
{
    png_zs_t *z = calloc(1, sizeof(*z));
    if (z != NULL && inflateInit(&z->s) != Z_OK) {
        free(z);
        z = NULL;
    }
    return z;
}

int png_zs_feed(png_zs_t *z, const uint8_t *in, size_t len, bool (*out)(void *ctx, const uint8_t *p, size_t n),
                void *ctx)
{
    z->s.next_in = (Bytef *)in;
    z->s.avail_in = (uInt)len;
    while (!z->done) {
        z->s.next_out = z->buf;
        z->s.avail_out = sizeof(z->buf);
        const int r = inflate(&z->s, Z_NO_FLUSH);
        const size_t n = sizeof(z->buf) - z->s.avail_out;
        if (n > 0 && !out(ctx, z->buf, n)) {
            return -1;
        }
        if (r == Z_STREAM_END) {
            z->done = true;
        } else if (r != Z_OK && r != Z_BUF_ERROR) {
            return -1;
        } else if (z->s.avail_in == 0 && z->s.avail_out > 0) {
            return 0;
        }
    }
    return 1;
}

void png_zs_close(png_zs_t *z)
{
    if (z != NULL) {
        inflateEnd(&z->s);
        free(z);
    }
}
