#include <string.h>

#include "cache_util.h"
#include "check.h"

#define PLAUSIBLE 1672531200

static void test_names(void)
{
    char a[13], b[13];
    cache_file_name("https://i.scdn.co/image/ab67616d00001e02", a);
    cache_file_name("https://i.scdn.co/image/ab67616d00001e03", b);
    CHECK_INT((int)strlen(a), 12);
    CHECK(strcmp(a + 8, ".BIN") == 0);
    CHECK(strcmp(a, b) != 0);
    for (int i = 0; i < 8; i++) {
        CHECK((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'A' && a[i] <= 'F'));
    }
    cache_file_name("https://i.scdn.co/image/ab67616d00001e02", b);
    CHECK_STR(a, b);
}

static void test_head(void)
{
    uint8_t h[CACHE_HEAD_MAX];
    const char *key = "rain1 nordic 1763125200 10 20 300 200 1";
    const size_t hl = cache_head_make(h, key, 1763125300, 60000, 0xDEADBEEF);
    CHECK_INT((int)hl, CACHE_HEAD_FIXED + (int)strlen(key));
    uint32_t saved = 0, len = 0, crc = 0;
    size_t got = 0;
    CHECK(cache_head_check(h, hl, hl + 60000, key, &saved, &len, &crc, &got));
    CHECK(saved == 1763125300 && len == 60000 && crc == 0xDEADBEEF && got == hl);
    /* Read with more of the file after it, as sd_cache.c does. */
    CHECK(cache_head_check(h, sizeof(h), hl + 60000, key, &saved, &len, &crc, &got));
    /* Another key that hashes to the same file. */
    CHECK(!cache_head_check(h, hl, hl + 60000, "rain1 nordic 1763125200 10 20 300 200 2", &saved, &len, &crc, &got));
    CHECK(!cache_head_check(h, hl, hl + 60000, "rain1 nordic", &saved, &len, &crc, &got));
    /* Cut short by the power going, or longer than it says. */
    CHECK(!cache_head_check(h, hl, hl + 59999, key, &saved, &len, &crc, &got));
    CHECK(!cache_head_check(h, hl, hl + 60001, key, &saved, &len, &crc, &got));
    CHECK(!cache_head_check(h, 19, hl + 60000, key, &saved, &len, &crc, &got));
    CHECK(!cache_head_check(h, hl - 1, hl + 60000, key, &saved, &len, &crc, &got));
    h[0] = 'X';
    CHECK(!cache_head_check(h, hl, hl + 60000, key, &saved, &len, &crc, &got));
    /* Keys: none, and too long. */
    CHECK_INT((int)cache_head_make(h, "", 0, 1, 0), 0);
    char lng[CACHE_KEY_MAX + 2];
    memset(lng, 'a', sizeof(lng) - 1);
    lng[sizeof(lng) - 1] = '\0';
    CHECK_INT((int)cache_head_make(h, lng, 0, 1, 0), 0);
    lng[CACHE_KEY_MAX] = '\0';
    CHECK_INT((int)cache_head_make(h, lng, 0, 1, 0), CACHE_HEAD_MAX);
    CHECK(cache_head_check(h, CACHE_HEAD_MAX, CACHE_HEAD_MAX + 1, lng, &saved, &len, &crc, &got));
    /* No data is no file. */
    cache_head_make(h, key, 0, 0, 0);
    CHECK(!cache_head_check(h, hl, hl, key, &saved, &len, &crc, &got));
}

static void test_fresh(void)
{
    const int64_t now = 1763125300;
    const uint32_t month = 30 * 24 * 3600;
    CHECK(cache_fresh((uint32_t)now - 100, now, month, PLAUSIBLE));
    CHECK(cache_fresh((uint32_t)(now - month), now, month, PLAUSIBLE));
    CHECK(!cache_fresh((uint32_t)(now - month - 1), now, month, PLAUSIBLE));
    CHECK(cache_fresh(1000, now, 0, PLAUSIBLE)); /* kept for good */
    /* No clock then, or now: used. */
    CHECK(cache_fresh(0, now, month, PLAUSIBLE));
    CHECK(cache_fresh((uint32_t)now, 5000, month, PLAUSIBLE));
    /* Saved over a day "after" now: a clock set wrong then. */
    CHECK(cache_fresh((uint32_t)now + 3600, now, month, PLAUSIBLE));
    CHECK(!cache_fresh((uint32_t)now + 2 * 86400, now, month, PLAUSIBLE));
}

static void test_prune(void)
{
    cache_entry_t e[10];
    for (int i = 0; i < 10; i++) {
        e[i].stamp = (uint32_t)(100 - i); /* newest first, as a listing may come */
        e[i].size = 1000;
        e[i].name[0] = (char)('A' + i);
        e[i].name[1] = '\0';
    }
    CHECK_INT(cache_prune_count(e, 10, 10, 10000, 80), 0); /* at both caps */
    CHECK_INT(cache_prune_count(e, 10, 9, 100000, 80), 3); /* 10 files to 7 (80 % of 9) */
    /* The oldest first: J (stamp 91), I, H. */
    CHECK(e[0].name[0] == 'J' && e[1].name[0] == 'I' && e[2].name[0] == 'H');
    CHECK_INT(cache_prune_count(e, 10, 100, 9999, 80), 3); /* 10000 bytes to 7999 or less */
    CHECK_INT(cache_prune_count(e, 0, 1, 1, 80), 0);
    /* One file bigger than the whole cap goes. */
    e[0].size = 50000;
    CHECK_INT(cache_prune_count(e, 1, 10, 10000, 80), 1);
}

void test_cache(void)
{
    test_names();
    test_head();
    test_fresh();
    test_prune();
}
