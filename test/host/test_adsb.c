#include "check.h"
#include "../../components/adsb_client.c"

void test_adsb(void)
{
    static const char json[] =
        "{\"ac\":["
        "{\"hex\":\"478123\",\"flight\":\"SAS83G  \",\"t\":\"B738\",\"alt_baro\":35000,\"gs\":450.5,"
        "\"track\":91.2,\"lat\":60.1,\"lon\":11.0,\"dst\":20.0,\"dir\":45.0,"
        "\"lastPosition\":{\"lat\":1,\"lon\":2},\"seen_pos\":1.5},"
        "{\"hex\":\"4acb01\",\"r\":\"LN-ABC\",\"alt_baro\":\"ground\",\"lat\":60,\"lon\":11,\"dst\":1,\"dir\":0},"
        "{\"hex\":\"4acb02\",\"alt_baro\":2500,\"lat\":60,\"lon\":11,\"dst\":5.0,\"dir\":180,\"true_heading\":270},"
        "{\"hex\":\"4acb03\",\"flight\":\"NOZ632\",\"lat\":60,\"dst\":2,\"dir\":10}"
        "],\"now\":1790000000}";
    static adsb_result_t out;
    memset(&out, 0, sizeof(out));
    CHECK(parse_response(json, strlen(json), &out));
    CHECK_INT(out.total, 2);                        /* one on the ground, one without a position */
    CHECK_INT(out.count, 2);
    CHECK_STR(out.ac[0].callsign, "4acb02");        /* nearest first; hex when no flight or reg */
    CHECK_INT(out.ac[0].alt_ft, 2500);
    CHECK((int)out.ac[0].track_deg == 270);         /* heading when no track */
    CHECK_STR(out.ac[1].callsign, "SAS83G");        /* trimmed */
    CHECK_STR(out.ac[1].type, "B738");
    CHECK((int)(out.ac[1].dist_km * 10) == 370);    /* 20 NM */
    CHECK(!parse_response("{}", 2, &out));
}
