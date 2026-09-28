#include "check.h"
#include "../../components/entur_client.c"

/* A Journey Planner reply for one stop: R31 north (via Nittedal's next
 * stops) and south, a bus in both directions, and a bus line not asked for. */
static const char REPLY[] =
    "{\"data\":{\"s0\":{\"id\":\"NSR:StopPlace:58858\",\"name\":\"Nittedal stasjon\",\"estimatedCalls\":["
    /* R31 northbound: calls at 502 after this quay */
    "{\"realtime\":true,\"cancellation\":false,\"aimedDepartureTime\":\"2026-09-28T20:14:00+02:00\","
    "\"expectedDepartureTime\":\"2026-09-28T20:15:00+02:00\",\"destinationDisplay\":{\"frontText\":\"Jaren\"},"
    "\"quay\":{\"id\":\"NSR:Quay:1\",\"publicCode\":\"1\"},\"serviceJourney\":{\"directionType\":\"unknown\","
    "\"line\":{\"id\":\"VYG:Line:R31\",\"publicCode\":\"R31\",\"transportMode\":\"rail\","
    "\"presentation\":{\"colour\":\"E60000\",\"textColour\":\"FFFFFF\"}},"
    "\"quays\":[{\"id\":\"NSR:Quay:9\",\"stopPlace\":{\"id\":\"NSR:StopPlace:530\"}},"
    "{\"id\":\"NSR:Quay:1\",\"stopPlace\":{\"id\":\"NSR:StopPlace:58858\"}},"
    "{\"id\":\"NSR:Quay:7\",\"stopPlace\":{\"id\":\"NSR:StopPlace:502\"}}]}},"
    /* R31 southbound: 502 comes before this quay, 530 after */
    "{\"realtime\":true,\"cancellation\":true,\"aimedDepartureTime\":\"2026-09-28T20:20:00+02:00\","
    "\"expectedDepartureTime\":\"2026-09-28T20:20:00+02:00\",\"destinationDisplay\":{\"frontText\":\"Oslo S\"},"
    "\"quay\":{\"id\":\"NSR:Quay:2\",\"publicCode\":\"2\"},\"serviceJourney\":{\"directionType\":\"unknown\","
    "\"line\":{\"id\":\"VYG:Line:R31\",\"publicCode\":\"R31\",\"transportMode\":\"rail\"},"
    "\"quays\":[{\"id\":\"NSR:Quay:7\",\"stopPlace\":{\"id\":\"NSR:StopPlace:502\"}},"
    "{\"id\":\"NSR:Quay:2\",\"stopPlace\":{\"id\":\"NSR:StopPlace:58858\"}},"
    "{\"id\":\"NSR:Quay:9\",\"stopPlace\":{\"id\":\"NSR:StopPlace:530\"}}]}},"
    /* bus 385 inbound, three calls (two kept) */
    "{\"aimedDepartureTime\":\"2026-09-28T20:01:00+02:00\",\"expectedDepartureTime\":\"2026-09-28T20:01:00+02:00\","
    "\"destinationDisplay\":{\"frontText\":\"Oslo\"},\"quay\":{\"id\":\"NSR:Quay:3\"},\"serviceJourney\":"
    "{\"directionType\":\"inbound\",\"line\":{\"id\":\"RUT:Line:385\",\"publicCode\":\"385\",\"transportMode\":\"bus\"}}},"
    "{\"aimedDepartureTime\":\"2026-09-28T20:11:00+02:00\",\"expectedDepartureTime\":\"2026-09-28T20:11:00+02:00\","
    "\"destinationDisplay\":{\"frontText\":\"Oslo\"},\"quay\":{\"id\":\"NSR:Quay:3\"},\"serviceJourney\":"
    "{\"directionType\":\"inbound\",\"line\":{\"id\":\"RUT:Line:385\",\"publicCode\":\"385\",\"transportMode\":\"bus\"}}},"
    "{\"aimedDepartureTime\":\"2026-09-28T20:21:00+02:00\",\"expectedDepartureTime\":\"2026-09-28T20:21:00+02:00\","
    "\"destinationDisplay\":{\"frontText\":\"Oslo\"},\"quay\":{\"id\":\"NSR:Quay:3\"},\"serviceJourney\":"
    "{\"directionType\":\"inbound\",\"line\":{\"id\":\"RUT:Line:385\",\"publicCode\":\"385\",\"transportMode\":\"bus\"}}},"
    /* bus 385 outbound: not asked for */
    "{\"aimedDepartureTime\":\"2026-09-28T20:05:00+02:00\",\"expectedDepartureTime\":\"2026-09-28T20:05:00+02:00\","
    "\"destinationDisplay\":{\"frontText\":\"Hakadal\"},\"quay\":{\"id\":\"NSR:Quay:4\"},\"serviceJourney\":"
    "{\"directionType\":\"outbound\",\"line\":{\"id\":\"RUT:Line:385\",\"publicCode\":\"385\",\"transportMode\":\"bus\"}}},"
    /* bus 390: not listed at all */
    "{\"aimedDepartureTime\":\"2026-09-28T20:06:00+02:00\",\"expectedDepartureTime\":\"2026-09-28T20:06:00+02:00\","
    "\"destinationDisplay\":{\"frontText\":\"X\"},\"quay\":{\"id\":\"NSR:Quay:4\"},\"serviceJourney\":"
    "{\"directionType\":\"outbound\",\"line\":{\"id\":\"RUT:Line:390\",\"publicCode\":\"390\",\"transportMode\":\"bus\"}}}"
    "]}}}";

void test_entur(void)
{
    static entur_selection_t sel;
    CHECK(entur_parse_selection("58858=VYG:Line:R31/v502+999,RUT:Line:385/in", &sel));
    CHECK_INT(sel.stop_count, 1);
    CHECK_STR(sel.stops[0].stop_id, "NSR:StopPlace:58858");
    CHECK_INT(sel.stops[0].line_count, 2);
    CHECK_STR(sel.stops[0].lines[0].id, "VYG:Line:R31");
    CHECK_INT(sel.stops[0].lines[0].via_count, 2);
    CHECK_INT(sel.stops[0].lines[0].via[0], 502);
    CHECK_INT(sel.stops[0].lines[0].via[1], 999);
    CHECK_INT(sel.stops[0].lines[1].dirs, ENTUR_DIR_IN);

    static entur_selection_t s2;
    CHECK(entur_parse_selection(" 6505 ; NSR:StopPlace:58366=RUT:Line:31/out ", &s2));
    CHECK_INT(s2.stop_count, 2);
    CHECK_STR(s2.stops[0].stop_id, "NSR:StopPlace:6505");
    CHECK_INT(s2.stops[0].line_count, 0);
    CHECK_INT(s2.stops[1].lines[0].dirs, ENTUR_DIR_OUT);
    CHECK(!entur_parse_selection("", &s2));
    CHECK(!entur_parse_selection(";;", &s2));

    /* Only lines listed; R31 only via 502 (north); 385 inbound only. */
    static entur_selection_t s3;
    CHECK(entur_parse_selection("58858=VYG:Line:R31/v502,RUT:Line:385/in", &s3));
    static entur_departures_t out;
    memset(&out, 0, sizeof(out));
    CHECK(parse_response(REPLY, &s3, &out));
    CHECK(out.valid);
    CHECK_STR(out.stop_name[0], "Nittedal stasjon");
    int r31 = -1, b385 = -1, b390 = -1;
    for (int g = 0; g < out.group_count; g++) {
        if (strcmp(out.groups[g].code, "R31") == 0) r31 = g;
        if (strcmp(out.groups[g].code, "385") == 0) b385 = g;
        if (strcmp(out.groups[g].code, "390") == 0) b390 = g;
    }
    CHECK(r31 >= 0 && b385 >= 0);
    CHECK(b390 >= 0);              /* lines are picked by the query (whiteListed), not here */
    if (r31 >= 0) {
        CHECK_INT(out.groups[r31].call_count, 1);          /* the southbound one filtered out */
        CHECK_STR(out.groups[r31].dest, "Jaren");
        CHECK_INT(out.groups[r31].calls[0].expected - out.groups[r31].calls[0].aimed, 60);
        CHECK(out.groups[r31].has_colour && out.groups[r31].colour == 0xE60000);
    }
    if (b385 >= 0) {
        CHECK_INT(out.groups[b385].call_count, ENTUR_PER_GROUP);
        CHECK_STR(out.groups[b385].dest, "Oslo");
    }

    /* Every line: R31 then shows both ways, grouped by destination. */
    static entur_selection_t all;
    CHECK(entur_parse_selection("58858", &all));
    memset(&out, 0, sizeof(out));
    CHECK(parse_response(REPLY, &all, &out));
    int r31_rows = 0, cancelled = 0;
    for (int g = 0; g < out.group_count; g++) {
        if (strcmp(out.groups[g].code, "R31") == 0) {
            r31_rows++;
            cancelled += out.groups[g].calls[0].cancelled;
        }
    }
    CHECK_INT(r31_rows, 2);
    CHECK_INT(cancelled, 1);
    CHECK_INT(out.group_count, 5); /* R31 x2, 385 in, 385 out, 390 */

    CHECK(!parse_response("not json", &all, &out));

    /* The query asks only for the listed lines, and for the via stops' ids
     * only when a line has a via direction. */
    char *q = build_query(&s3);
    CHECK(q != NULL);
    CHECK(strstr(q, "whiteListed:{lines:[") != NULL && strstr(q, "RUT:Line:385") != NULL);
    CHECK(strstr(q, "stopPlace{id}") != NULL);
    free(q);
    q = build_query(&all);
    CHECK(q != NULL && strstr(q, "whiteListed") == NULL && strstr(q, "stopPlace{id}") == NULL);
    free(q);
}
