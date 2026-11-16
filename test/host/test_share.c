#include <string.h>

#include "check.h"
#include "share_util.h"

static void test_signed(void)
{
    /* SHA-256 of nothing, as server/deling/deling.py signs a GET's body. */
    static const uint8_t empty[32] = { 0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4,
                                       0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b,
                                       0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
    char out[256];
    const size_t n = share_signed_text("GET", "bilder?grupper=familien", 1794000000, "00ff", empty, out, sizeof(out));
    CHECK_STR(out, "vaertavla-deling-1\nGET\nbilder?grupper=familien\n1794000000\n00ff\n"
                   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_INT(n, strlen(out));
    CHECK_INT(share_signed_text("GET", "bilder", 1, "00ff", empty, out, 60), 0);
    CHECK_STR(out, "");
}

static void test_ids(void)
{
    CHECK(share_id_ok("0123456789ab"));
    CHECK(!share_id_ok("0123456789AB"));
    CHECK(!share_id_ok("0123456789a"));
    CHECK(!share_id_ok("0123456789abc"));
    CHECK(!share_id_ok("../../etc/pa"));
    char name[24], id[SHARE_ID_LEN + 1];
    share_file_name("a1b2c3d4e5f6", name, sizeof(name));
    CHECK_STR(name, "Da1b2c3d4e5f6.JPG");
    CHECK(share_file_id(name, id));
    CHECK_STR(id, "a1b2c3d4e5f6");
    CHECK(share_file_id("DA1B2C3D4E5F6.jpg", id));
    CHECK_STR(id, "a1b2c3d4e5f6");
    CHECK(!share_file_id("B0000001.JPG", id));
    CHECK(!share_file_id("Da1b2c3d4e5f6.TMP", id));
    CHECK(!share_file_id("Dsommerhytta.JPG", id));
    CHECK(!share_file_id("Da1b2c3d4e5f6x.JPG", id));
}

static void test_groups(void)
{
    CHECK(share_group_ok("familien"));
    CHECK(share_group_ok("hytte-venner_2"));
    CHECK(!share_group_ok(""));
    CHECK(!share_group_ok("Familien"));
    CHECK(!share_group_ok("bl\xc3\xa5"));
    CHECK(!share_group_ok("a,b"));
    CHECK(!share_group_ok("abcdefghijklmnopqrstuvwxy")); /* 25 */
    char out[SHARE_GROUPS_MAX];
    CHECK(share_groups_clean(" Familien, venner;hytta  venner ", out, sizeof(out)));
    CHECK_STR(out, "familien,venner,hytta");
    CHECK(share_groups_clean("", out, sizeof(out)));
    CHECK_STR(out, "");
    CHECK(share_groups_clean(" , ", out, sizeof(out)));
    CHECK_STR(out, "");
    CHECK(share_groups_clean("fam,familien", out, sizeof(out)));
    CHECK_STR(out, "fam,familien");
    CHECK(!share_groups_clean("familien, bl\xc3\xa5", out, sizeof(out)));
    CHECK_STR(out, "");
    CHECK(!share_groups_clean("abcdefghijklmnopqrstuvwxyz", out, sizeof(out)));
    char small[12];
    CHECK(share_groups_clean("abc,def", small, sizeof(small)));
    CHECK_STR(small, "abc,def");
    CHECK(!share_groups_clean("abc,def,ghij", small, sizeof(small)));
}

static void test_encode(void)
{
    char out[32];
    CHECK(share_url_encode("Hytta p\xc3\xa5 Sand", out, sizeof(out)));
    CHECK_STR(out, "Hytta%20p%C3%A5%20Sand");
    CHECK(share_url_encode("a&b=c", out, sizeof(out)));
    CHECK_STR(out, "a%26b%3Dc");
    CHECK(share_url_encode("", out, sizeof(out)));
    CHECK_STR(out, "");
    char small[6];
    CHECK(share_url_encode("abcde", small, sizeof(small)));
    CHECK(!share_url_encode("abc d", small, sizeof(small)));
    CHECK_STR(small, "");
}

static void test_nick(void)
{
    char out[SHARE_NICK_MAX], id[SHARE_ID_LEN + 1];
    share_nick_clean("  Bestemor  p\xc3\xa5\tHytta \n", out, sizeof(out));
    CHECK_STR(out, "Bestemor p\xc3\xa5 Hytta");
    share_nick_clean("", out, sizeof(out));
    CHECK_STR(out, "");
    share_nick_clean("a\x7f\x01" "b\xff" "c", out, sizeof(out));
    CHECK_STR(out, "a bc");
    /* 39 a's and an "\xc3\xa5" don't fit in 40: the whole character left out. */
    share_nick_clean("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xc3\xa5", out, sizeof(out));
    CHECK_INT(strlen(out), 39);
    share_nick_clean("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", out, sizeof(out));
    CHECK_INT(strlen(out), 40);
    char small[6];
    share_nick_clean("ab cdef", small, sizeof(small));
    CHECK_STR(small, "ab cd");
    CHECK(share_nick_line("a1b2c3d4e5f6\tMormor\r\n", id, out));
    CHECK_STR(id, "a1b2c3d4e5f6");
    CHECK_STR(out, "Mormor");
    CHECK(share_nick_line("a1b2c3d4e5f6\t", id, out));
    CHECK_STR(out, "");
    CHECK(!share_nick_line("a1b2c3d4e5f6 Mormor", id, out));
    CHECK(!share_nick_line("A1B2C3D4E5F6\tMormor", id, out));
    CHECK(!share_nick_line("", id, out));
}

void test_share(void)
{
    test_nick();
    test_encode();
    test_signed();
    test_ids();
    test_groups();
}
