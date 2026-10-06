#include <stdlib.h>
#include "check.h"
#include "../../components/aurora_client.c"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    b[fread(b, 1, n, f)] = '\0';
    fclose(f);
    return b;
}

void test_aurora(void)
{
    /* Yr's reply for Kvaløysletta, 18:00 to 06:00 one night: aurora value
     * 0.33 from 23:00, but only 01:00 with the sky partly clear (56 %). */
    static aurora_t a;
    char *json = slurp("data/aurora.json");
    CHECK(json != NULL);
    if (json) {
        CHECK(aurora_parse(json, &a));
        CHECK_INT(a.count, 12);
        CHECK_INT(a.hours[0].start, iso8601_to_epoch("2026-10-06T16:00:00Z"));
        CHECK_INT(a.hours[0].end - a.hours[0].start, 3600);
        CHECK(a.hours[0].dark && a.hours[0].cloud == 100 && a.hours[0].value == 0.0f); /* dusk */
        int good = 0, at = -1;
        for (int i = 0; i < a.count; i++) {
            if (aurora_good(&a.hours[i])) {
                good++;
                at = i;
            }
        }
        CHECK_INT(good, 1);
        CHECK_INT(at, 7);
        CHECK_INT(a.hours[7].kp, 3);
        free(json);
    }
    CHECK(!aurora_parse("{\"status\":{}}", &a));
    CHECK(!aurora_parse("not json", &a));
    aurora_hour_t h = { .value = 0.5f, .cloud = 20, .dark = false };
    CHECK(!aurora_good(&h)); /* daylight */
    h.dark = true;
    CHECK(aurora_good(&h));
    h.cloud = 80;
    CHECK(!aurora_good(&h)); /* overcast */
}
