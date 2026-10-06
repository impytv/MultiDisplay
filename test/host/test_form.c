#include "check.h"
#include "../../components/form_util.c"

void test_form(void)
{
    char v[16];
    CHECK(form_field("a=1&name=Oslo+S&b=2", "name", v, sizeof(v)));
    CHECK_STR(v, "Oslo S");
    CHECK(form_field("name1=x&name=y", "name", v, sizeof(v)));
    CHECK_STR(v, "y");                                     /* not a prefix match */
    CHECK(!form_field("a=1", "name", v, sizeof(v)));
    CHECK_STR(v, "");
    CHECK(form_field("name=", "name", v, sizeof(v)));
    CHECK_STR(v, "");
    CHECK(form_field("q=%C3%B8st%2F1", "q", v, sizeof(v)));
    CHECK_STR(v, "\xC3\xB8st/1");
    CHECK(form_field("q=100%zz", "q", v, sizeof(v)));      /* not an escape: kept as is */
    CHECK_STR(v, "100%zz");
    CHECK(form_field("q=50%", "q", v, sizeof(v)));
    CHECK_STR(v, "50%");

    /* The limit is on the decoded text: 14 letters, each 6 bytes encoded. */
    char big[256] = "n=";
    for (int i = 0; i < 14; i++) {
        strcat(big, "%C3%A6");
    }
    char w[16];
    CHECK(form_field(big, "n", w, sizeof(w)));
    CHECK_INT(strlen(w), 14);                              /* 7 whole "æ", not 7.5 */
    for (size_t i = 0; i < strlen(w); i += 2) {
        CHECK((unsigned char)w[i] == 0xC3 && (unsigned char)w[i + 1] == 0xA6);
    }
    char w3[4];
    CHECK(form_field("n=a%C3%A6", "n", w3, sizeof(w3)));
    CHECK_STR(w3, "a\xC3\xA6");
    char w2[3];
    CHECK(form_field("n=a%C3%A6", "n", w2, sizeof(w2)));
    CHECK_STR(w2, "a");                                    /* half an "æ" left off */

    /* buf_append stops at the end, and stays there. */
    char b[8];
    char *bp = b, *be = b + sizeof(b);
    bp = buf_append(bp, be, "%s", "abc");
    CHECK_INT(bp - b, 3);
    bp = buf_append(bp, be, "%d", 12345);
    CHECK(bp == be - 1);
    CHECK_STR(b, "abc1234");
    bp = buf_append(bp, be, "more");
    CHECK(bp == be - 1);
    CHECK_STR(b, "abc1234");
    char c1[1];
    CHECK(buf_append(c1, c1 + 1, "x") == c1);
}
