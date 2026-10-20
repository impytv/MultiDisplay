#include <stdlib.h>
#include "check.h"
#include "../../components/png_decode.c"
#include "data/png/expected.h"

/* Pictures made by make_png_data.py: every colour type and bit depth, each
 * row filter, transparency three ways, shrinking and growing, and one as
 * an image library writes it. */
static uint8_t *slurp_png(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    *len = fread(b, 1, (size_t)n, f);
    fclose(f);
    return b;
}

void test_png(void)
{
    static uint16_t out[64 * 64];
    size_t len;
    for (size_t i = 0; i < sizeof(PNG_CASES) / sizeof(PNG_CASES[0]); i++) {
        const png_case_t *c = &PNG_CASES[i];
        uint8_t *png = slurp_png(c->file, &len);
        CHECK(png != NULL && png_is(png, len));
        memset(out, 0, sizeof(out));
        const bool ok = png != NULL && png_decode_square(png, len, out, c->side, 0xFFFFFF);
        CHECK(ok);
        int bad = -1;
        for (int k = 0; ok && k < c->side * c->side && bad < 0; k++) {
            bad = out[k] == c->px[k] ? -1 : k;
        }
        if (bad >= 0) {
            printf("  %s: pixel %d is %04x, not %04x\n", c->file, bad, out[bad], c->px[bad]);
        }
        CHECK(bad < 0);
        free(png);
    }
    /* What it can't read. */
    const char *const BAD[] = { "data/png/interlaced.png", "data/png/cut.png" };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        uint8_t *png = slurp_png(BAD[i], &len);
        CHECK(png != NULL && !png_decode_square(png, len, out, 8, 0xFFFFFF));
        free(png);
    }
    static const uint8_t JPEG[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F', 'I', 'F' };
    CHECK(!png_is(JPEG, sizeof(JPEG)));
    CHECK(!png_decode_square(JPEG, sizeof(JPEG), out, 8, 0));
}
