#include "check.h"
#include "../../components/adsb_owners.c"

static void scan_in_pieces(owner_scan_t *s, const char *page, size_t piece)
{
    memset(s, 0, sizeof(*s));
    for (size_t i = 0, n = strlen(page); i < n; i += piece) {
        scan_feed(s, page + i, n - i < piece ? n - i : piece);
    }
}

void test_owners(void)
{
    CHECK(is_norwegian("LN-ORA"));
    CHECK(!is_norwegian("LN-"));
    CHECK(!is_norwegian("SE-ABC"));
    CHECK(!is_norwegian(""));

    /* Operator and owner from the register page's escaped JSON, however the
     * page is split up as it arrives. */
    static const char page[] =
        "<div id=\"registration\" data-react-props=\"{&quot;mark&quot;:&quot;LN-ORA&quot;,"
        "&quot;operator&quot;:null,&quot;mainOwner&quot;:&quot;STATEN v/ POLITIDIREKTORATET&quot;,"
        "&quot;owners&quot;:[{&quot;name&quot;:&quot;Someone else&quot;}],"
        "&quot;organizationalNumberOperator&quot;:null}\"></div>";
    static const size_t pieces[] = { 1, 3, 7, 64, sizeof(page) };
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
        owner_scan_t s;
        scan_in_pieces(&s, page, pieces[i]);
        CHECK(scan_done(&s));
        CHECK_STR(s.f[FIELD_OPERATOR].value, "");
        CHECK_STR(s.f[FIELD_OWNER].value, "STATEN v/ POLITIDIREKTORATET");
    }
    owner_scan_t s;
    scan_in_pieces(&s, "&quot;operator&quot;:&quot;Norsk Luftambulanse AS&quot;,"
                       "mainOwnemainOwner&quot;:&quot;Skandinaviska Enskilda Banken AB (publ)&quot;", 5);
    CHECK(scan_done(&s));
    CHECK_STR(s.f[FIELD_OPERATOR].value, "Norsk Luftambulanse AS");
    CHECK_STR(s.f[FIELD_OWNER].value, "Skandinaviska Enskilda Banken AB (publ)"); /* after a near miss */
    /* A long owner is cut short, not overrun. */
    scan_in_pieces(&s, "mainOwner&quot;:&quot;AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA&quot;", 4);
    CHECK(s.f[FIELD_OWNER].state == DONE);
    CHECK(strlen(s.f[FIELD_OWNER].value) == sizeof(s.f[FIELD_OWNER].value) - 1);
    /* No entry on the page. */
    scan_in_pieces(&s, "<html>Ingen treff</html>", 4);
    CHECK(!scan_done(&s));

    CHECK(role_of_name("STATEN v/ POLITIDIREKTORATET") == ADSB_ROLE_POLICE);
    CHECK(role_of_name("Norsk Luftambulanse AS") == ADSB_ROLE_AMBULANCE);
    CHECK(role_of_name("Stiftelsen Norsk Luftambulanse") == ADSB_ROLE_AMBULANCE);
    CHECK(role_of_name("Nordic Air Ambulance A/S") == ADSB_ROLE_AMBULANCE);
    CHECK(role_of_name("Babcock Scandinavian AirAmbulance AB") == ADSB_ROLE_AMBULANCE);
    CHECK(role_of_name("Widerøe's Flyveselskap AS") == ADSB_ROLE_NONE);
    CHECK(role_of_name("") == ADSB_ROLE_NONE);

    CHECK(role_of_callsign("SAVER41") == ADSB_ROLE_RESCUE);
    CHECK(role_of_callsign("DOC02") == ADSB_ROLE_AMBULANCE);
    CHECK(role_of_callsign("BNO92J") == ADSB_ROLE_AMBULANCE);
    CHECK(role_of_callsign("BNOL") == ADSB_ROLE_NONE); /* no digit after the prefix */
    CHECK(role_of_callsign("DOCTOR1") == ADSB_ROLE_NONE);
    CHECK(role_of_callsign("SAS4417") == ADSB_ROLE_NONE);
    CHECK(role_of_callsign("") == ADSB_ROLE_NONE);

    /* Lookups fail on the host (no network): only callsigns give a role. */
    static adsb_result_t r;
    memset(&r, 0, sizeof(r));
    r.count = 3;
    strcpy(r.ac[0].callsign, "HEP01");
    strcpy(r.ac[0].reg, "LN-ORA");
    r.ac[0].role = ADSB_ROLE_POLICE;
    strcpy(r.ac[1].callsign, "SAAB12");
    strcpy(r.ac[1].reg, "SE-ABC");
    r.ac[1].role = ADSB_ROLE_POLICE;
    strcpy(r.ac[2].callsign, "SAVER40");
    CHECK(adsb_owners_fill(&r, 2) != ESP_OK);
    CHECK(r.ac[0].role == ADSB_ROLE_NONE);
    CHECK(r.ac[1].role == ADSB_ROLE_NONE); /* reset even when not looked up */
    CHECK(r.ac[2].role == ADSB_ROLE_RESCUE);
}
