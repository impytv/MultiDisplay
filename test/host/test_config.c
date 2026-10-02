#include <stdlib.h>
#include "check.h"
#include "../../components/app_config.h"
#include "cJSON.h"

void test_config(void)
{
    test_nvs_clear();
    static app_config_t a, b;
    app_config_load(&a);                            /* the Kconfig defaults */
    CHECK_INT(a.location_count, 1);
    CHECK_STR(a.locations[0].name, "Oslo");
    CHECK_INT(a.ota_auto, 0);
    CHECK_STR(a.device_name, "multidisplay");

    /* Save and load again: everything comes back. */
    snprintf(a.wifi_ssid, sizeof(a.wifi_ssid), "home");
    snprintf(a.wifi_pass, sizeof(a.wifi_pass), "secret1");
    snprintf(a.web_pass, sizeof(a.web_pass), "secret2");
    snprintf(a.ais_client_secret, sizeof(a.ais_client_secret), "secret3");
    snprintf(a.ais_client_id, sizeof(a.ais_client_id), "me@example.com:client");
    a.location_count = 2;
    snprintf(a.locations[1].name, sizeof(a.locations[1].name), "Kval\xC3\xB8ysletta");
    snprintf(a.locations[1].lat, sizeof(a.locations[1].lat), "69.6957");
    snprintf(a.locations[1].lon, sizeof(a.locations[1].lon), "18.8837");
    a.show[1] = APP_SHOW_WEATHER | APP_SHOW_SHIPS;
    a.ship_km[1] = 30;
    snprintf(a.departures[1], sizeof(a.departures[1]), "58858=VYG:Line:R31/v502");
    a.night_off = 1;
    a.dim_start = 22 * 60 + 23;
    a.ota_auto = 1;
    a.show[0] = APP_SHOW_WEATHER | APP_SHOW_TIDE;
    a.cal_show = 1;
    a.cal_rotate = 1;
    snprintf(a.cal_url[0], sizeof(a.cal_url[0]), "https://calendar.google.com/calendar/ical/x/private-secret4/basic.ics");
    snprintf(a.cal_url[2], sizeof(a.cal_url[2]), "webcal://p01-caldav.icloud.com/published/2/secret5");
    CHECK(app_config_save(&a) == ESP_OK);
    CHECK(app_config_is_provisioned());
    app_config_load(&b);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);

    /* The backup leaves out the WiFi and the secrets. */
    char *json = app_config_to_json(&a);
    CHECK(json != NULL);
    CHECK(strstr(json, "secret") == NULL);
    CHECK(strstr(json, "\"home\"") == NULL);
    CHECK(strstr(json, "22:23") != NULL);

    /* Restored onto another display: its WiFi and secrets stay. */
    static app_config_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.wifi_ssid, sizeof(c.wifi_ssid), "other");
    snprintf(c.wifi_pass, sizeof(c.wifi_pass), "otherpass");
    snprintf(c.ais_client_id, sizeof(c.ais_client_id), "me@example.com:client");
    snprintf(c.ais_client_secret, sizeof(c.ais_client_secret), "kept");
    c.location_count = 1;
    char err[96];
    CHECK(app_config_from_json(json, &c, err, sizeof(err)));
    CHECK_STR(c.wifi_ssid, "other");
    CHECK_STR(c.wifi_pass, "otherpass");
    CHECK_STR(c.ais_client_secret, "kept");         /* same client id: the secret stays */
    CHECK_INT(c.location_count, 2);
    CHECK_STR(c.locations[1].name, "Kval\xC3\xB8ysletta");
    CHECK_INT(c.ship_km[1], 30);
    CHECK_STR(c.departures[1], "58858=VYG:Line:R31/v502");
    CHECK_INT(c.show[0], APP_SHOW_WEATHER | APP_SHOW_TIDE);
    CHECK(c.cal_show && c.cal_rotate && c.cal_url[0][0] == '\0'); /* the calendar addresses aren't in it */

    CHECK(app_config_cal_url_valid("https://outlook.office365.com/owa/calendar/a/b/calendar.ics"));
    CHECK(app_config_cal_url_valid("webcal://x.example/cal.ics"));
    CHECK(!app_config_cal_url_valid("ftp://x.example/cal.ics"));
    CHECK(!app_config_cal_url_valid("https://x.example/a b.ics"));
    CHECK(!app_config_cal_url_valid(""));
    CHECK_INT(c.night_off, 1);
    CHECK_INT(c.dim_start, 22 * 60 + 23);
    CHECK_INT(c.ota_auto, 1);

    /* A different client id drops the secret, which belonged to the old one. */
    snprintf(c.ais_client_id, sizeof(c.ais_client_id), "someone-else");
    CHECK(app_config_from_json(json, &c, err, sizeof(err)));
    CHECK_STR(c.ais_client_secret, "");
    cJSON_free(json);

    /* Refused, and nothing changed. */
    static app_config_t before;
    before = c;
    CHECK(!app_config_from_json("{\"format\":\"other\"}", &c, err, sizeof(err)));
    CHECK(!app_config_from_json("not json", &c, err, sizeof(err)));
    CHECK(!app_config_from_json("{\"format\":\"multidisplay-innstillinger\",\"version\":99}", &c, err, sizeof(err)));
    CHECK(!app_config_from_json("{\"format\":\"multidisplay-innstillinger\",\"version\":1,\"theme\":1,"
                                "\"locations\":[{\"name\":\"X\",\"lat\":\"95\",\"lon\":\"10\"}]}",
                                &c, err, sizeof(err)));
    CHECK(strstr(err, "Sted 1") != NULL);
    CHECK(!app_config_from_json("{\"format\":\"multidisplay-innstillinger\",\"version\":1,\"locations\":[]}",
                                &c, err, sizeof(err)));
    CHECK(memcmp(&before, &c, sizeof(c)) == 0);

    /* Display names become host names. */
    char host[APP_CONFIG_DEVNAME_MAX];
    app_config_hostname("Kj\xC3\xB8kken", host, sizeof(host));
    CHECK_STR(host, "kjokken");
    app_config_hostname("  Stua 2. etg ", host, sizeof(host));
    CHECK_STR(host, "stua-2-etg");
    app_config_hostname("\xC3\x86rfugl_\xC3\x85sen!", host, sizeof(host));
    CHECK_STR(host, "aerfugl-aasen");
    app_config_hostname("--", host, sizeof(host));
    CHECK_STR(host, "");
    char small[6];
    app_config_hostname("abcd-efgh", small, sizeof(small));
    CHECK_STR(small, "abcd");                       /* no '-' left at the end */

    /* The name is saved, but stays out of a backup and isn't restored. */
    snprintf(c.device_name, sizeof(c.device_name), "kjokken");
    CHECK(app_config_save(&c) == ESP_OK);
    static app_config_t d;
    app_config_load(&d);
    CHECK_STR(d.device_name, "kjokken");
    char *j2 = app_config_to_json(&d);
    CHECK(j2 != NULL && strstr(j2, "kjokken") == NULL);
    snprintf(d.device_name, sizeof(d.device_name), "stua");
    CHECK(app_config_from_json(j2, &d, err, sizeof(err)));
    CHECK_STR(d.device_name, "stua");
    cJSON_free(j2);
    snprintf(d.device_name, sizeof(d.device_name), "Bad Name!");
    app_config_sanitize(&d);
    CHECK_STR(d.device_name, "bad-name");

    /* Out-of-range values are brought back in range. */
    CHECK(app_config_from_json("{\"format\":\"multidisplay-innstillinger\",\"version\":1,"
                               "\"fonts\":{\"title_px\":200},\"locations\":[{\"name\":\"X\",\"lat\":\"60\","
                               "\"lon\":\"10\",\"aircraft_km\":5000,\"show\":0}]}", &c, err, sizeof(err)));
    CHECK_INT(c.title_px, APP_CONFIG_TITLE_PX_DEFAULT);
    CHECK_INT(c.radar_km[0], APP_CONFIG_RADAR_KM_DEFAULT);
    CHECK_INT(c.show[0], APP_SHOW_WEATHER);
    CHECK_INT(c.location_count, 1);
}
