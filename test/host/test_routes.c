#include "check.h"
#include "../../components/adsb_routes.c"

void test_routes(void)
{
    CHECK(is_flight("SAS4417"));
    CHECK(is_flight("NOZ632"));
    CHECK(is_flight("SAS83G"));
    CHECK(!is_flight("LN-ABC"));
    CHECK(!is_flight("4acb02"));
    CHECK(!is_flight("AB1"));

    /* Lookups fail on the host (no network): nothing is cached or shown. */
    static adsb_result_t r;
    memset(&r, 0, sizeof(r));
    r.count = 2;
    strcpy(r.ac[0].callsign, "SAS4417");
    strcpy(r.ac[1].callsign, "LN-ABC");
    strcpy(r.ac[1].route, "junk");
    CHECK(adsb_routes_fill(&r, 14, 4) != ESP_OK);
    CHECK_STR(r.ac[0].route, "");
    CHECK_STR(r.ac[1].route, "");                   /* reset even when not looked up */
}
