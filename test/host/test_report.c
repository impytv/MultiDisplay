#include <string.h>
#include "check.h"
#include "../../components/report_util.c"

void test_report(void)
{
    /* The signed text, as server/feil/feil.py builds it. */
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) {
        sha[i] = (uint8_t)(i * 9);
    }
    char text[REPORT_SIGNED_MAX];
    report_signed_text(1793790000, sha, text);
    CHECK_STR(text, "vaertavla-feil-1\n1793790000\n"
                    "0009121b242d363f48515a636c757e879099a2abb4bdc6cfd8e1eaf3fc050e17");
    report_signed_text(INT64_MAX, sha, text); /* the longest fits */
    CHECK(strlen(text) == 17 + 19 + 1 + 64);

    char b[71];
    CHECK(report_boundary("multipart/form-data; boundary=----WebKitFormBoundaryAbC123", b, sizeof(b)));
    CHECK_STR(b, "----WebKitFormBoundaryAbC123");
    CHECK(report_boundary("Multipart/Form-Data; BOUNDARY=\"a b\"; x=y", b, sizeof(b)));
    CHECK_STR(b, "a b");
    CHECK(report_boundary("multipart/form-data; boundary=abc; charset=x", b, sizeof(b)));
    CHECK_STR(b, "abc");
    CHECK(!report_boundary("text/plain; boundary=abc", b, sizeof(b)));
    CHECK(!report_boundary("multipart/form-data", b, sizeof(b)));
    CHECK(!report_boundary("multipart/form-data; boundary=", b, sizeof(b)));
    CHECK(!report_boundary("multipart/form-data; boundary=\"abc", b, sizeof(b)));
    CHECK(!report_boundary("multipart/form-data; boundary=a\r\nb", b, sizeof(b)));
    char seventy[128] = "multipart/form-data; boundary=";
    memset(seventy + strlen(seventy), 'x', 70);
    CHECK(report_boundary(seventy, b, sizeof(b)));
    CHECK(!report_boundary(seventy, b, 70)); /* no room for the '\0' */
    strcat(seventy, "x");
    CHECK(!report_boundary(seventy, b, sizeof(b)));
}
