#include <stdlib.h>

#include "check.h"
#include "pic_util.h"

static uint32_t s_seed = 1;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return s_seed >> 8;
}

static uint32_t zero(void)
{
    return 0;
}

static void test_names(void)
{
    CHECK(pic_is_picture("B0000001.JPG"));
    CHECK(pic_is_picture("photo~1.jpe"));
    CHECK(pic_is_picture("IMG_0001.Jpg"));
    CHECK(!pic_is_picture("B0000001.TMP"));
    CHECK(!pic_is_picture("README.TXT"));
    CHECK(!pic_is_picture("JPG"));
    CHECK(!pic_is_picture(".JPG"));
    CHECK(!pic_is_picture("._IMG.JPG")); /* a Mac's resource fork */
    CHECK(!pic_is_picture("IMG.JPGX"));
    /* Long names, as exFAT and FAT with long names list them. */
    CHECK(pic_is_picture("Sommer p\xc3\xa5 hytta 2024.jpeg"));
    CHECK(pic_is_picture("PXL_20240612_154233123.MP.jpg"));
    CHECK(!pic_is_picture("._Sommer p\xc3\xa5 hytta.jpg"));
    CHECK(!pic_is_picture("film.jpg.mov"));
    CHECK_INT(pic_number("B0000042.JPG.jpg"), -1);

    /* What /bilder/hent passes on. */
    static const uint8_t jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F', 'I', 'F', 0, 1 };
    static const uint8_t png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13 };
    static const uint8_t bmp[] = { 'B', 'M', 0x36, 0x10, 0x0E, 0, 0, 0, 0, 0, 0x36, 0, 0, 0, 40, 0, 0, 0 };
    static const uint8_t bm_text[] = "BMW 320d til salgs";
    static const uint8_t heic[] = { 0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c', 0, 0, 0, 0 };
    static const uint8_t avif[] = { 0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f', 0, 0, 0, 0 };
    static const uint8_t mp4[] = { 0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0 };
    static const uint8_t mov[] = { 0, 0, 0, 0x14, 'f', 't', 'y', 'p', 'q', 't', ' ', ' ', 0, 0, 0, 0 };
    static const uint8_t html[] = "<!doctype html><html>";
    CHECK(pic_looks_like_picture(jpg, sizeof(jpg)));
    CHECK(pic_looks_like_picture(png, sizeof(png)));
    CHECK(pic_looks_like_picture((const uint8_t *)"GIF89a\x01\0\x01\0\0\0", 12));
    CHECK(pic_looks_like_picture((const uint8_t *)"RIFF\x10\0\0\0WEBPVP8 ", 16));
    CHECK(pic_looks_like_picture(bmp, sizeof(bmp)));
    CHECK(!pic_looks_like_picture(bmp, 17)); /* too short to tell */
    CHECK(!pic_looks_like_picture(bm_text, sizeof(bm_text) - 1));
    CHECK(pic_looks_like_picture(heic, sizeof(heic)));
    CHECK(pic_looks_like_picture(avif, sizeof(avif)));
    CHECK(!pic_looks_like_picture(mp4, sizeof(mp4)));
    CHECK(!pic_looks_like_picture(mov, sizeof(mov)));
    CHECK(!pic_looks_like_picture(html, sizeof(html) - 1));
    CHECK(!pic_looks_like_picture(jpg, 2));

    CHECK_INT(pic_number("B0000042.JPG"), 42);
    CHECK_INT(pic_number("b0001234.jpg"), 1234);
    CHECK_INT(pic_number("B9999999.JPG"), 9999999);
    CHECK_INT(pic_number("B0000000.JPG"), -1);
    CHECK_INT(pic_number("B000004.JPG"), -1);
    CHECK_INT(pic_number("B0000042.TMP"), -1);
    CHECK_INT(pic_number("IMG_0042.JPG"), -1);
    CHECK_INT(pic_number("B00000A2.JPG"), -1);

    char buf[PIC_NAME_MAX];
    pic_name(buf, 7, "JPG");
    CHECK_STR(buf, "B0000007.JPG");
    pic_name(buf, 1234567, "TMP");
    CHECK_STR(buf, "B1234567.TMP");
    CHECK_INT(pic_number(buf), -1);
    pic_name(buf, 9999999, "JPG");
    CHECK_INT(pic_number(buf), 9999999);
}

static void test_fit(void)
{
    int w = 0, h = 0;
    /* Made by the page: as it is. */
    CHECK(pic_fit(800, 480, 800, 480, &w, &h) && w == 800 && h == 480);
    CHECK(pic_fit(360, 480, 800, 480, &w, &h) && w == 360 && h == 480);
    CHECK(pic_fit(17, 9, 800, 480, &w, &h) && w == 17 && h == 9);
    /* A 12 MP photo copied on: 4000x3000 to 640x480. */
    CHECK(pic_fit(4000, 3000, 800, 480, &w, &h) && w == 640 && h == 480);
    /* 16:9 from a camera: limited by the width. */
    CHECK(pic_fit(1920, 1080, 800, 480, &w, &h) && w == 800 && h == 448); /* 450 down to 448 */
    /* Portrait. A 12 MP one is beyond 1/8 to be 480 high. */
    CHECK(pic_fit(2400, 3200, 800, 480, &w, &h) && w == 360 && h == 480);
    CHECK(!pic_fit(3000, 4000, 800, 480, &w, &h));
    CHECK(w % 8 == 0 && h % 8 == 0);
    /* Just over: scaled a little. */
    CHECK(pic_fit(801, 480, 800, 480, &w, &h) && w == 800 && h == 472);
    /* Beyond 1/8: 8000 wide fits at 800, a tenth. */
    CHECK(!pic_fit(8000, 4800, 800, 480, &w, &h));
    CHECK(pic_fit(6400, 3840, 800, 480, &w, &h) && w == 800 && h == 480);
    /* A thin strip would be under 8 high. */
    CHECK(!pic_fit(4000, 30, 800, 480, &w, &h));
    CHECK(!pic_fit(0, 480, 800, 480, &w, &h));
    CHECK(!pic_fit(800, -1, 800, 480, &w, &h));
}

static void test_bag(void)
{
    uint16_t order[50];
    pic_bag_t b = { .order = order, .cap = 50 };
    pic_bag_reset(&b, 0);
    CHECK_INT(pic_bag_next(&b, rnd), -1);

    pic_bag_reset(&b, 1);
    CHECK_INT(pic_bag_next(&b, rnd), 0);
    CHECK_INT(pic_bag_next(&b, rnd), 0);

    /* Each once per round, and not the same twice in a row across rounds. */
    pic_bag_reset(&b, 10);
    int prev = -1;
    bool repeat = false, all_rounds = true;
    for (int round = 0; round < 50; round++) {
        int seen[10] = { 0 };
        for (int k = 0; k < 10; k++) {
            const int i = pic_bag_next(&b, rnd);
            if (i < 0 || i >= 10) {
                all_rounds = false;
                continue;
            }
            seen[i]++;
            repeat |= i == prev;
            prev = i;
        }
        for (int i = 0; i < 10; i++) {
            all_rounds &= seen[i] == 1;
        }
    }
    CHECK(all_rounds);
    CHECK(!repeat);

    /* With a "random" that always gives 0 the shuffle puts the last one
     * first next round; the swap must move it. */
    pic_bag_reset(&b, 3);
    int a = pic_bag_next(&b, zero), c = pic_bag_next(&b, zero), d = pic_bag_next(&b, zero);
    const int e = pic_bag_next(&b, zero);
    CHECK(a != c && c != d && a != d);
    CHECK(e != d);

    /* After the card is listed again (pictures.c, list_card) the one on
     * show is set as last: the new order never starts with it. */
    bool first_again = false;
    for (int k = 0; k < 200; k++) {
        pic_bag_reset(&b, 2 + k % 5);
        b.last = k % 2;
        first_again |= pic_bag_next(&b, rnd) == k % 2;
    }
    CHECK(!first_again);

    /* More than it holds: capped. */
    pic_bag_reset(&b, 80);
    CHECK_INT(b.n, 50);
    for (int k = 0; k < 120; k++) {
        const int i = pic_bag_next(&b, rnd);
        CHECK(i >= 0 && i < 50);
    }
}

void test_pics(void)
{
    test_names();
    test_fit();
    test_bag();
}
