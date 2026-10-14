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

    /* Each callsign is asked for once, whatever the answer; only a request
     * that got no answer is tried again. */
    adsb_routes_close();
    g_http_stub = (http_stub_t){ .on = true, .status = 200, .body = "{\"_airport_codes_iata\":\"TOS-OSL\"}" };
    r.count = 1;
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_OK);
    CHECK_STR(r.ac[0].route, "TOS-OSL");
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_OK);
    CHECK_STR(r.ac[0].route, "TOS-OSL");            /* from the cache */
    CHECK_INT(g_http_stub.requests, 1);

    g_http_stub.status = 404;                       /* no route known */
    strcpy(r.ac[0].callsign, "NOZ632");
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_OK);
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_OK);
    CHECK_STR(r.ac[0].route, "");
    CHECK_INT(g_http_stub.requests, 2);

    g_http_stub.status = 503;                       /* an error, but kept */
    strcpy(r.ac[0].callsign, "WIF123");
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_ERR_INVALID_RESPONSE);
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_OK);
    CHECK_INT(g_http_stub.requests, 3);

    g_http_stub.err = ESP_ERR_TIMEOUT;              /* no answer: asked again */
    strcpy(r.ac[0].callsign, "DLH456");
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_ERR_TIMEOUT);
    g_http_stub.err = ESP_OK;
    g_http_stub.status = 404;
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_OK);
    CHECK(adsb_routes_fill(&r, 14, 4) == ESP_OK);
    CHECK_INT(g_http_stub.requests, 5);
    adsb_routes_close();
    g_http_stub = (http_stub_t){ 0 };
}
