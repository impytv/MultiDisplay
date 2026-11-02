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

/* Nittedal stasjon toward Oslo (via Kjelsås, 530) with replacement buses
 * (2 November 2026): one by Sinsen to Oslo S, which stops at no station
 * but comes well nearer to Kjelsås; one the other way, to Hakadal; and the
 * train after them. Coordinates rounded off Entur's. */
#define BUS_JOURNEY(sub, quays)                                                         \
    "\"serviceJourney\":{\"directionType\":\"unknown\",\"transportSubmode\":\"" sub "\"," \
    "\"line\":{\"id\":\"VYG:Line:R31\",\"publicCode\":\"R31\",\"transportMode\":\"rail\"},\"quays\":[" quays "]}}"
#define NITTEDAL_TRACK "{\"id\":\"NSR:Quay:2\",\"latitude\":60.0476,\"longitude\":10.8706,\"stopPlace\":{\"id\":\"NSR:StopPlace:58858\"}}"
#define NITTEDAL_BUS   "{\"id\":\"NSR:Quay:5\",\"latitude\":60.0478,\"longitude\":10.8712,\"stopPlace\":{\"id\":\"NSR:StopPlace:58858\"}}"
static const char REPLACED[] =
    "{\"data\":{\"v530\":{\"latitude\":59.9653,\"longitude\":10.7811},"
    "\"s0\":{\"name\":\"Nittedal stasjon\",\"estimatedCalls\":["
    "{\"realtime\":true,\"aimedDepartureTime\":\"2026-11-02T07:41:00+01:00\",\"expectedDepartureTime\":\"2026-11-02T07:41:00+01:00\","
    "\"destinationDisplay\":{\"frontText\":\"Oslo S\"},\"quay\":{\"id\":\"NSR:Quay:5\",\"publicCode\":\"B\"},"
    BUS_JOURNEY("railReplacementBus", NITTEDAL_BUS ","
                "{\"id\":\"NSR:Quay:61\",\"latitude\":59.9284,\"longitude\":10.7787,\"stopPlace\":{\"id\":\"NSR:StopPlace:6435\"}},"
                "{\"id\":\"NSR:Quay:62\",\"latitude\":59.9112,\"longitude\":10.7570,\"stopPlace\":{\"id\":\"NSR:StopPlace:62\"}}") ","
    "{\"realtime\":true,\"aimedDepartureTime\":\"2026-11-02T07:45:00+01:00\",\"expectedDepartureTime\":\"2026-11-02T07:45:00+01:00\","
    "\"destinationDisplay\":{\"frontText\":\"Hakadal\"},\"quay\":{\"id\":\"NSR:Quay:5\"},"
    BUS_JOURNEY("railReplacementBus", NITTEDAL_BUS ","
                "{\"id\":\"NSR:Quay:71\",\"latitude\":60.0702,\"longitude\":10.8455,\"stopPlace\":{\"id\":\"NSR:StopPlace:71\"}},"
                "{\"id\":\"NSR:Quay:72\",\"latitude\":60.1129,\"longitude\":10.8293,\"stopPlace\":{\"id\":\"NSR:StopPlace:72\"}}") ","
    "{\"realtime\":true,\"aimedDepartureTime\":\"2026-11-02T08:19:00+01:00\",\"expectedDepartureTime\":\"2026-11-02T08:19:00+01:00\","
    "\"destinationDisplay\":{\"frontText\":\"Oslo S\"},\"quay\":{\"id\":\"NSR:Quay:2\",\"publicCode\":\"2\"},"
    BUS_JOURNEY("local", NITTEDAL_TRACK ","
                "{\"id\":\"NSR:Quay:9\",\"latitude\":59.9653,\"longitude\":10.7811,\"stopPlace\":{\"id\":\"NSR:StopPlace:530\"}}")
    "]}}}";

void test_entur(void)
{
    char *q;
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
    /* Flybussen's line IDs end in a UUID. */
    CHECK(entur_parse_selection("6038=RUT:Line:21/out,NBU:Line:a7c15243-8873-4aee-a8b4-8d22e8ad31dc/v58211", &s2));
    CHECK_INT(s2.stops[0].line_count, 2);
    CHECK_STR(s2.stops[0].lines[1].id, "NBU:Line:a7c15243-8873-4aee-a8b4-8d22e8ad31dc");
    CHECK_INT(s2.stops[0].lines[1].via_count, 1);
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

    /* The user's headings: R31 north under "Til jobb", 385 under "Hjem";
     * the third name has no line, and "#9" no name. */
    static entur_selection_t sec;
    CHECK(entur_parse_selection(" #Til jobb | Hjem|Ubrukt ;58858=VYG:Line:R31/v502#1,RUT:Line:385/in#2,"
                                "RUT:Line:390#9", &sec));
    CHECK_INT(sec.stop_count, 1);
    CHECK_INT(sec.section_count, 3);
    CHECK_STR(sec.section[0], "Til jobb");
    CHECK_STR(sec.section[1], "Hjem");
    CHECK_INT(sec.stops[0].lines[0].section, 1);
    CHECK_INT(sec.stops[0].lines[0].via_count, 1);
    CHECK_INT(sec.stops[0].lines[1].section, 2);
    CHECK_INT(sec.stops[0].lines[1].dirs, ENTUR_DIR_IN);
    CHECK_INT(sec.stops[0].lines[2].section, 0);
    memset(&out, 0, sizeof(out));
    CHECK(parse_response(REPLY, &sec, &out));
    CHECK_STR(out.section[0], "Til jobb");
    CHECK_STR(out.section[2], ""); /* no line under it */
    CHECK(out.stop_rest[0]);       /* 390 */
    CHECK_INT(out.group_count, 3);
    CHECK(strcmp(out.groups[0].code, "R31") == 0 && out.groups[0].section == 1);
    CHECK(strcmp(out.groups[1].code, "385") == 0 && out.groups[1].section == 2);
    CHECK(strcmp(out.groups[2].code, "390") == 0 && out.groups[2].section == 0);

    /* One line in both directions, each under its own heading; R31's two
     * via directions likewise. Asked for once each in the query. */
    CHECK(entur_parse_selection("#Til jobb|Hjem;58858=RUT:Line:385/in#1,RUT:Line:385/out#2,"
                                "VYG:Line:R31/v502#1,VYG:Line:R31/v530#2", &sec));
    memset(&out, 0, sizeof(out));
    CHECK(parse_response(REPLY, &sec, &out));
    CHECK(!out.stop_rest[0]);
    CHECK_INT(out.group_count, 5); /* and 390, left to the query to drop */
    int under[3] = { 0 };
    for (int g = 0; g < out.group_count; g++) {
        under[out.groups[g].section]++;
        if (out.groups[g].section == 0) {
            CHECK_STR(out.groups[g].code, "390");
        } else if (strcmp(out.groups[g].code, "385") == 0) {
            CHECK_STR(out.groups[g].dest, out.groups[g].section == 1 ? "Oslo" : "Hakadal");
        } else {
            CHECK_STR(out.groups[g].dest, out.groups[g].section == 1 ? "Jaren" : "Oslo S");
        }
    }
    CHECK(under[1] == 2 && under[2] == 2);
    q = build_query(&sec);
    CHECK(q != NULL && strstr(q, "[\"RUT:Line:385\",\"VYG:Line:R31\"]") != NULL);
    free(q);

    /* A long heading is cut at a whole character. */
    char longsel[128] = "#";
    for (int k = 0; k < 40; k++) {
        strcat(longsel, "\xC3\xB8");
    }
    strcat(longsel, ";6505");
    CHECK(entur_parse_selection(longsel, &sec));
    CHECK_INT(strlen(sec.section[0]), 62);

    /* Disruption notices: the Norwegian text, summary and description. */
    static const char SIT[] =
        "{\"data\":{\"s0\":{\"name\":\"Oslo S\",\"estimatedCalls\":["
        "{\"aimedDepartureTime\":\"2026-09-28T20:01:00+02:00\",\"expectedDepartureTime\":\"2026-09-28T20:01:00+02:00\","
        "\"destinationDisplay\":{\"frontText\":\"Eidsvoll\"},\"quay\":{\"id\":\"q\"},"
        "\"serviceJourney\":{\"directionType\":\"outbound\",\"line\":{\"id\":\"VYG:Line:L1\",\"publicCode\":\"L1\"}},"
        "\"situations\":[{\"summary\":[{\"value\":\"Fewer cars\",\"language\":\"en\"},"
        "{\"value\":\"F\xC3\xA6rre vogner\",\"language\":\"no\"}],"
        "\"description\":[{\"value\":\"Denne avgangen kj\xC3\xB8rer med 4 vogner.\",\"language\":\"no\"}]}]},"
        "{\"aimedDepartureTime\":\"2026-09-28T20:05:00+02:00\",\"expectedDepartureTime\":\"2026-09-28T20:05:00+02:00\","
        "\"destinationDisplay\":{\"frontText\":\"Spikkestad\"},\"quay\":{\"id\":\"q\"},"
        "\"serviceJourney\":{\"directionType\":\"inbound\",\"line\":{\"id\":\"VYG:Line:L1\",\"publicCode\":\"L1\"}},"
        "\"situations\":[{\"summary\":[{\"value\":\"Forsinket\",\"language\":\"nob\"}],\"description\":[]}]},"
        "{\"aimedDepartureTime\":\"2026-09-28T20:07:00+02:00\",\"expectedDepartureTime\":\"2026-09-28T20:07:00+02:00\","
        "\"destinationDisplay\":{\"frontText\":\"Lillestr\xC3\xB8m\"},\"quay\":{\"id\":\"q\"},"
        "\"serviceJourney\":{\"directionType\":\"outbound\",\"line\":{\"id\":\"VYG:Line:R10\",\"publicCode\":\"R10\"}},"
        "\"situations\":[]}]}}}";
    static entur_selection_t os;
    CHECK(entur_parse_selection("59872", &os));
    memset(&out, 0, sizeof(out));
    CHECK(parse_response(SIT, &os, &out));
    CHECK_INT(out.group_count, 3);
    for (int g = 0; g < out.group_count; g++) {
        if (strcmp(out.groups[g].dest, "Eidsvoll") == 0) {
            CHECK_STR(out.groups[g].notice, "F\xC3\xA6rre vogner: Denne avgangen kj\xC3\xB8rer med 4 vogner.");
        } else if (strcmp(out.groups[g].dest, "Spikkestad") == 0) {
            CHECK_STR(out.groups[g].notice, "Forsinket");
        } else {
            CHECK_STR(out.groups[g].notice, "");
        }
    }

    /* The query asks only for the listed lines, and for the via stops' ids
     * only when a line has a via direction. */
    q = build_query(&s3);
    CHECK(q != NULL);
    CHECK(strstr(q, "whiteListed:{lines:[") != NULL && strstr(q, "RUT:Line:385") != NULL);
    CHECK(strstr(q, "stopPlace{id}") != NULL);
    CHECK(strstr(q, "situations{summary{value language} description{value language}}") != NULL);
    free(q);
    q = build_query(&all);
    CHECK(q != NULL && strstr(q, "whiteListed") == NULL && strstr(q, "stopPlace{id}") == NULL);
    free(q);

    /* Replacement buses: the one toward Kjelsås kept, in orange with "Buss
     * for tog", the train's track kept for the row; the other way dropped. */
    static entur_selection_t rb;
    CHECK(entur_parse_selection("58858=VYG:Line:R31/v530", &rb));
    memset(&out, 0, sizeof(out));
    CHECK(parse_response(REPLACED, &rb, &out));
    CHECK_INT(out.group_count, 1);
    CHECK_INT(out.groups[0].call_count, 2);
    CHECK(out.groups[0].calls[0].replacement_bus);
    CHECK(!out.groups[0].calls[1].replacement_bus);
    CHECK_STR(out.groups[0].quay, "2");
    CHECK_STR(out.groups[0].notice, "Buss for tog");
    q = build_query(&rb);
    CHECK(q != NULL && strstr(q, "v530:stopPlace(id:\"NSR:StopPlace:530\"){latitude longitude}") != NULL);
    CHECK(strstr(q, "quays{id latitude longitude stopPlace{id}}") != NULL && strstr(q, "transportSubmode") != NULL);
    free(q);
    /* A via stop named on two lines is asked for once. */
    CHECK(entur_parse_selection("58858=VYG:Line:R31/v530,VYG:Line:RE30/v530", &rb));
    q = build_query(&rb);
    CHECK(q != NULL && strstr(q, "v530:") != NULL && strstr(strstr(q, "v530:") + 1, "v530:") == NULL);
    free(q);
}
