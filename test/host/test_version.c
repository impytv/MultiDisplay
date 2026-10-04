#include "check.h"
#include "../../components/version_util.c"
#include "../../components/update_schedule.h"

void test_version(void)
{
    CHECK(version_compare("1.10.0", "1.9.3") > 0);
    CHECK(version_compare("1.0.0", "1.0.0") == 0);
    CHECK(version_compare("1.0.0", "1.0.1") < 0);
    CHECK(version_compare("2", "1.99.99") > 0);
    CHECK(version_compare("1.2.0-3-gabc", "1.2.0") == 0);  /* git-describe suffix ignored */
    CHECK(version_compare("garbage", "0.0.1") < 0);        /* never newer */
    CHECK(version_compare("0.0.1", "garbage") > 0);

    char out[128];
    url_resolve("http://192.168.0.119:8070/manifest.json", "firmware/a.bin", out, sizeof(out));
    CHECK_STR(out, "http://192.168.0.119:8070/firmware/a.bin");
    url_resolve("http://h/x/manifest-test.json", "firmware/a.bin", out, sizeof(out));
    CHECK_STR(out, "http://h/x/firmware/a.bin");
    url_resolve("http://h/x/m.json", "/fw/a.bin", out, sizeof(out));
    CHECK_STR(out, "http://h/fw/a.bin");
    url_resolve("http://h", "a.bin", out, sizeof(out));
    CHECK_STR(out, "http://h/a.bin");
    url_resolve("http://h/m.json", "https://other/a.bin", out, sizeof(out));
    CHECK_STR(out, "https://other/a.bin");

    uint8_t b[32];
    CHECK(hex_to_bytes32("75a6d939b109d7233a22c7315b42db4dd1c88b7ee200b3477f5ca8ac02757ea5", b));
    CHECK_INT(b[0], 0x75);
    CHECK_INT(b[31], 0xa5);
    CHECK(hex_to_bytes32("75A6D939B109D7233A22C7315B42DB4DD1C88B7EE200B3477F5CA8AC02757EA5", b));
    CHECK(!hex_to_bytes32("75a6", b));
    CHECK(!hex_to_bytes32("0x a6d939b109d7233a22c7315b42db4dd1c88b7ee200b3477f5ca8ac02757ea5", b));
    CHECK(!hex_to_bytes32(NULL, b));

    /* The update schedule: 03:30 every 6 hours. */
    struct tm lt = { .tm_year = 2026 - 1900, .tm_mon = 9, .tm_mday = 4, .tm_hour = 3, .tm_min = 29 };
    int into;
    const int64_t n0 = upd_period(upd_local_min(&lt), 210, 6, &into);
    CHECK_INT(into, 359);                               /* the last minute of the period from 21:30 */
    lt.tm_min = 30;
    CHECK(upd_period(upd_local_min(&lt), 210, 6, &into) == n0 + 1);
    CHECK_INT(into, 0);
    CHECK_INT(upd_period_start_min(n0 + 1, 210, 6), 210);
    CHECK_INT(upd_period_start_min(n0 + 2, 210, 6), 9 * 60 + 30);
    CHECK_INT(upd_period_start_min(n0, 210, 6), 21 * 60 + 30);
    /* Every 24 hours: one period a day, starting at the time set. */
    lt.tm_hour = 2; lt.tm_min = 0;
    const int64_t d0 = upd_period(upd_local_min(&lt), 210, 24, &into);
    CHECK_INT(into, 22 * 60 + 30);
    lt.tm_mday = 5; lt.tm_hour = 3; lt.tm_min = 31;
    CHECK(upd_period(upd_local_min(&lt), 210, 24, &into) == d0 + 2);
    CHECK_INT(into, 1);
    CHECK_INT(upd_period_start_min(d0 + 5, 210, 24), 210);
    /* Every 7 hours wanders through the day. */
    CHECK_INT(upd_period_start_min(1, 0, 7), 7 * 60);
    CHECK_INT(upd_period_start_min(4, 0, 7), 4 * 60);
}
