/* PNG's zlib streams inflated on the display (see png_decode.h), with the
 * tinfl in the chip's ROM. Its state (~11 KB) and the 32 KB window it
 * writes into go to PSRAM. */

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "miniz.h"

#include "png_decode.h"

struct png_zs {
    tinfl_decompressor d;
    uint8_t dict[TINFL_LZ_DICT_SIZE];
    size_t at; /* where in dict the next output goes */
    bool done;
};

png_zs_t *png_zs_open(void)
{
    png_zs_t *z = heap_caps_malloc_prefer(sizeof(*z), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (z != NULL) {
        tinfl_init(&z->d);
        z->at = 0;
        z->done = false;
    }
    return z;
}

int png_zs_feed(png_zs_t *z, const uint8_t *in, size_t len, bool (*out)(void *ctx, const uint8_t *p, size_t n),
                void *ctx)
{
    while (!z->done) {
        size_t in_n = len, out_n = TINFL_LZ_DICT_SIZE - z->at;
        /* Always "more to come": the IDAT chunks are fed one by one, and the
         * stream says itself where it ends. */
        const tinfl_status st = tinfl_decompress(&z->d, in, &in_n, z->dict, z->dict + z->at, &out_n,
                                                 TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
        in += in_n;
        len -= in_n;
        if (out_n > 0 && !out(ctx, z->dict + z->at, out_n)) {
            return -1;
        }
        z->at = (z->at + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (st < 0) {
            return -1;
        }
        if (st == TINFL_STATUS_DONE) {
            z->done = true;
        } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && len == 0) {
            return 0;
        }
    }
    return 1;
}

void png_zs_close(png_zs_t *z)
{
    heap_caps_free(z);
}
