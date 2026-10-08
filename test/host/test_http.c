#include "check.h"
#include "../../components/http_util.c"

void test_http(void)
{
    CHECK_INT(iso8601_to_epoch("2026-09-27T20:31:00+02:00"), 1790533860);
    CHECK_INT(iso8601_to_epoch("2026-09-27T18:31:00Z"), 1790533860);
    CHECK_INT(iso8601_to_epoch("2026-09-27T18:31:00.123Z"), 1790533860);
    CHECK_INT(iso8601_to_epoch("2026-09-27T18:31:00"), 1790533860);
    CHECK_INT(iso8601_to_epoch("nonsense"), 0);
    CHECK_INT(iso8601_to_epoch(NULL), 0);
    /* Out of range: refused rather than overflowing. */
    CHECK_INT(iso8601_to_epoch("2026-13-01T00:00:00Z"), 0);
    CHECK_INT(iso8601_to_epoch("2026-09-27T18:31:00+99:00"), 0);
    CHECK_INT(iso8601_to_epoch("999999999-09-27T18:31:00Z"), 0);
    CHECK_INT(http_date_to_epoch("Mon, 28 Sep 99999 18:00:02 GMT"), 0);

    CHECK_INT(http_date_to_epoch("Mon, 28 Sep 2026 18:00:02 GMT"), 1790618402);
    CHECK_INT(http_date_to_epoch("Sun, 01 Mar 2026 00:00:00 GMT"), 1772323200);
    CHECK_INT(http_date_to_epoch("Mon, 28 Foo 2026 18:00:02 GMT"), 0);
    CHECK_INT(http_date_to_epoch(""), 0);

    http_buf_t b = { .max = 40000 };
    char chunk[1000];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 30; i++) {
        CHECK(http_buf_append(&b, chunk, sizeof(chunk), "t") == ESP_OK);
    }
    CHECK_INT(b.len, 30000);
    CHECK(b.buf[b.len] == '\0');
    CHECK(http_buf_append(&b, chunk, sizeof(chunk), "t") == ESP_OK);
    for (int i = 0; i < 9; i++) {
        http_buf_append(&b, chunk, sizeof(chunk), "t");
    }
    CHECK(http_buf_append(&b, chunk, sizeof(chunk), "t") != ESP_OK); /* over max */
    http_buf_reset(&b);
    CHECK_INT(b.len, 0);
    http_buf_free(&b);
    CHECK(b.buf == NULL);

    CHECK(http_retry_worthwhile(true, esp_timer_get_time()));
    CHECK(!http_retry_worthwhile(false, esp_timer_get_time()));
    CHECK(!http_retry_worthwhile(true, esp_timer_get_time() - 5000000));
}
