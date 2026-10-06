/* Aircraft routes by callsign, from the route data adsb.lol publishes (the
 * Virtual Radar Server "standing data"): one small JSON file per callsign,
 * https://vrs-standing-data.adsb.lol/routes/SA/SAS4417.json, whose
 * "_airport_codes_iata" is e.g. "TOS-OSL". Scheduled routes change rarely,
 * so each answer - including "no route known" - is kept for a day. */

#include "esp_attr.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "adsb_client.h"
#include "http_util.h"

static const char *TAG = "adsb_routes";

#define ROUTE_CACHE      96
#define ROUTE_KEEP_US    (24 * 3600 * 1000000LL)
#define ROUTE_TIMEOUT_MS 8000
#define ROUTE_MAX_BODY   (8 * 1024)

typedef struct {
    char callsign[9];
    char route[12];
    int64_t at_us; /* when looked up; 0 = free slot */
} route_entry_t;

static EXT_RAM_BSS_ATTR route_entry_t s_cache[ROUTE_CACHE];
static esp_http_client_handle_t s_client;
static http_buf_t s_body = { .max = ROUTE_MAX_BODY };

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    /* Only a route's JSON: an unknown callsign's 404 is a 10 KB error page. */
    if (evt->event_id != HTTP_EVENT_ON_DATA || esp_http_client_get_status_code(evt->client) != 200) {
        return ESP_OK;
    }
    return http_buf_append(&s_body, evt->data, evt->data_len, TAG);
}

/* Airline flights only - "SAS4417", "NOZ632": three letters and a digit.
 * Registrations and ICAO hex codes (the fallbacks) have no route. */
static bool is_flight(const char *cs)
{
    return isalpha((unsigned char)cs[0]) && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2]) &&
           isdigit((unsigned char)cs[3]);
}

static route_entry_t *find(const char *cs, int64_t now)
{
    for (int i = 0; i < ROUTE_CACHE; i++) {
        if (s_cache[i].at_us != 0 && now - s_cache[i].at_us < ROUTE_KEEP_US &&
            strcmp(s_cache[i].callsign, cs) == 0) {
            return &s_cache[i];
        }
    }
    return NULL;
}

/* The oldest slot (or a free one). */
static route_entry_t *slot(void)
{
    route_entry_t *e = &s_cache[0];
    for (int i = 1; i < ROUTE_CACHE && e->at_us != 0; i++) {
        if (s_cache[i].at_us < e->at_us) {
            e = &s_cache[i];
        }
    }
    return e;
}

/* Look `cs` up: ESP_OK with the route in `out` ("" if none is known). */
static esp_err_t lookup(const char *cs, char *out, size_t out_len)
{
    char url[96];
    snprintf(url, sizeof(url), "https://vrs-standing-data.adsb.lol/routes/%.2s/%s.json", cs, cs);
    if (s_client == NULL) {
        const esp_http_client_config_t cfg = {
            .url = url,
            .event_handler = on_event,
            .timeout_ms = ROUTE_TIMEOUT_MS,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) {
            return ESP_FAIL;
        }
        esp_http_client_set_header(s_client, "User-Agent", CONFIG_EXAMPLE_YR_USER_AGENT);
    } else {
        esp_http_client_set_url(s_client, url);
    }
    http_buf_reset(&s_body);
    esp_err_t err = esp_http_client_perform(s_client);
    const int status = esp_http_client_get_status_code(s_client);
    if (err != ESP_OK) {
        adsb_routes_close(); /* a fresh connection next time */
        return err;
    }
    out[0] = '\0';
    if (status == 404) {
        return ESP_OK; /* no route known for this callsign */
    }
    if (status != 200 || s_body.buf == NULL) {
        return ESP_FAIL;
    }
    json_use_psram();
    cJSON *root = cJSON_Parse(s_body.buf);
    const char *iata = cJSON_GetStringValue(cJSON_GetObjectItem(root, "_airport_codes_iata"));
    if (iata != NULL && strchr(iata, '-') != NULL) {
        snprintf(out, out_len, "%s", iata);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t adsb_routes_fill(adsb_result_t *res, int rows, int max_fetches)
{
    const int64_t now = esp_timer_get_time();
    esp_err_t result = ESP_OK;
    for (int i = 0; i < res->count && i < rows; i++) {
        adsb_aircraft_t *a = &res->ac[i];
        a->route[0] = '\0';
        if (!is_flight(a->callsign)) {
            continue;
        }
        const route_entry_t *hit = find(a->callsign, now);
        if (hit == NULL && max_fetches > 0) {
            max_fetches--;
            char route[sizeof(a->route)];
            esp_err_t err = lookup(a->callsign, route, sizeof(route));
            if (err == ESP_OK) {
                route_entry_t *e = slot();
                snprintf(e->callsign, sizeof(e->callsign), "%s", a->callsign);
                snprintf(e->route, sizeof(e->route), "%s", route);
                e->at_us = now;
                hit = e;
                ESP_LOGD(TAG, "%s: %s", a->callsign, route[0] ? route : "no route");
            } else {
                result = err;
                max_fetches = 0; /* the site is having trouble: try again next poll */
            }
        }
        if (hit != NULL) {
            snprintf(a->route, sizeof(a->route), "%s", hit->route);
        }
    }
    return result;
}

void adsb_routes_close(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    http_buf_free(&s_body);
}
