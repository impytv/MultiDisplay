#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "wifi_provision.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"

static const char *TAG = "wifi_provision";

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define STA_CONNECT_TIMEOUT_MS  45000
#define PORTAL_AP_IP            "192.168.4.1"
#define PORTAL_MAX_SCAN         20

#define BIT_CONNECTED  BIT0
#define BIT_GAVE_UP    BIT1

static EventGroupHandle_t s_events;
static int s_retries;
static bool s_stop_reconnect;
static httpd_handle_t s_httpd;
static wifi_provision_status_fn s_status;
static char s_ap_ssid[24];
static char s_sta_ip[16]; /* "" until the first IP_EVENT_STA_GOT_IP */

static void status(const char *msg)
{
    if (s_status) {
        s_status(msg);
    }
}

/* --------------------------------------------------------------------------
 * Small text helpers
 * ------------------------------------------------------------------------ */

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            int hi = r[1], lo = r[2];
            hi = (hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
            lo = (lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
            *w++ = (char)((hi << 4) | lo);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* Extract application/x-www-form-urlencoded field `key` from `body` into
 * `dst` (URL-decoded, NUL-terminated, truncated to dst_len). */
static bool form_field(const char *body, const char *key, char *dst, size_t dst_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *eq = strchr(p, '=');
        if (eq && (!amp || eq < amp) && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            const char *val = eq + 1;
            size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
            if (vlen >= dst_len) {
                vlen = dst_len - 1;
            }
            memcpy(dst, val, vlen);
            dst[vlen] = '\0';
            url_decode(dst);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    dst[0] = '\0';
    return false;
}

/* Append `src` to `dst` (bounded by dst_end), HTML-escaping " & < >. */
static char *html_escape_append(char *dst, char *dst_end, const char *src)
{
    for (; *src && dst < dst_end - 6; src++) {
        switch (*src) {
        case '"': dst += sprintf(dst, "&quot;"); break;
        case '&': dst += sprintf(dst, "&amp;"); break;
        case '<': dst += sprintf(dst, "&lt;"); break;
        case '>': dst += sprintf(dst, "&gt;"); break;
        default:  *dst++ = *src; break;
        }
    }
    *dst = '\0';
    return dst;
}

/* --------------------------------------------------------------------------
 * Config web page
 * ------------------------------------------------------------------------ */

static const char PAGE_HEAD[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>MultiDisplay setup</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:26rem;margin:2rem auto;padding:0 1rem;background:#f6f6f4;color:#222}"
    "h1{font-size:1.3rem}label{display:block;margin:.8rem 0 .2rem;font-weight:600}"
    "input{width:100%;box-sizing:border-box;padding:.5rem;font-size:1rem;border:1px solid #bbb;border-radius:.4rem}"
    "button{margin-top:1.3rem;width:100%;padding:.7rem;font-size:1rem;border:0;border-radius:.4rem;background:#2d5a86;color:#fff}"
    ".row{display:flex;gap:.6rem}.row>div{flex:1}small{color:#666}"
    "fieldset{margin:.9rem 0;padding:.2rem .8rem .8rem;border:1px solid #ccc;border-radius:.5rem}"
    "legend{padding:0 .4rem;color:#555;font-weight:600}"
    "select{width:100%;box-sizing:border-box;padding:.5rem;font-size:1rem;border:1px solid #bbb;border-radius:.4rem;background:#fff}"
    ".chk{display:flex;gap:1.2rem;flex-wrap:wrap}.chk label{display:flex;align-items:center;gap:.35rem;margin:.2rem 0;font-weight:400}"
    ".chk input{width:auto;margin:0}"
    "</style><h1>MultiDisplay setup</h1><form method=post action=/save>";

static const char PAGE_TAIL[] =
    "<button type=submit>Save &amp; restart</button></form>"
    "<script>fetch('/scan').then(r=>r.json()).then(l=>{let d=document.getElementById('nets');"
    "l.forEach(n=>{let o=document.createElement('option');o.value=n.s;d.appendChild(o)})}).catch(e=>{});</script>";

/* Build the full page into a heap buffer (caller frees). */
static char *build_page(const app_config_t *cfg)
{
    const size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    char *p = buf;
    char *end = buf + cap;

    p += snprintf(p, end - p, "%s", PAGE_HEAD);

    p += snprintf(p, end - p, "<label>WiFi network</label>"
                  "<input name=ssid list=nets autocomplete=off value=\"");
    p = html_escape_append(p, end, cfg->wifi_ssid);
    p += snprintf(p, end - p, "\"><datalist id=nets></datalist>");

    p += snprintf(p, end - p, "<label>WiFi password</label>"
                  "<input name=pass type=password autocomplete=off value=\"");
    p = html_escape_append(p, end, cfg->wifi_pass);
    p += snprintf(p, end - p, "\"><small>Leave blank for an open network</small>");

    p += snprintf(p, end - p,
                  "<label>Theme</label><select name=theme>"
                  "<option value=0%s>Light</option>"
                  "<option value=1%s>Dark</option></select>",
                  cfg->theme == APP_THEME_DARK ? "" : " selected",
                  cfg->theme == APP_THEME_DARK ? " selected" : "");

    p += snprintf(p, end - p,
                  "<label>Contact email for yr</label>"
                  "<input name=yremail type=email autocomplete=email value=\"");
    p = html_escape_append(p, end, cfg->yr_email);
    p += snprintf(p, end - p, "\"><small>Sent to api.met.no in the User-Agent header, "
                  "as their terms require. Leave blank to use the built-in "
                  "default.</small>");

    p += snprintf(p, end - p,
                  "<fieldset><legend>BarentsWatch (ship traffic)</legend>"
                  "<small>API client from barentswatch.no/minside, with access "
                  "to the AIS API. Only needed for ship traffic.</small>"
                  "<label>Client ID</label><input name=aisid autocomplete=off value=\"");
    p = html_escape_append(p, end, cfg->ais_client_id);
    p += snprintf(p, end - p, "\"><label>Client secret</label>"
                  "<input name=aissec type=password autocomplete=off value=\"");
    p = html_escape_append(p, end, cfg->ais_client_secret);
    p += snprintf(p, end - p, "\"><label>Minimum ship length (m)</label>"
                  "<input name=shipminlen type=number inputmode=numeric min=0 max=%d value=%u>"
                  "<small>Shorter ships, and ships that don't report a length, are "
                  "hidden. 0 shows every ship.</small></fieldset>",
                  APP_CONFIG_SHIP_MIN_LEN_MAX, cfg->ship_min_len_m);

    p += snprintf(p, end - p,
                  "<p style='margin:1.4rem 0 .2rem'><small>One or more "
                  "locations. The screen shows one at a time; tap the right "
                  "half for the next, the left half for the previous. Leave a "
                  "block empty to skip it. For each location choose any of its "
                  "weather, a live aircraft radar and live ship traffic (shown "
                  "in that order), and how far the radar and ship traffic "
                  "look.</small>");

    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        bool filled = (i < cfg->location_count);
        p += snprintf(p, end - p,
                      "<fieldset><legend>Location %d</legend>"
                      "<label>Name</label><input name=name%d value=\"", i + 1, i);
        if (filled) {
            p = html_escape_append(p, end, cfg->locations[i].name);
        }
        p += snprintf(p, end - p, "\"><div class=row><div><label>Latitude</label>"
                      "<input name=lat%d inputmode=decimal value=\"", i);
        if (filled) {
            p = html_escape_append(p, end, cfg->locations[i].lat);
        }
        p += snprintf(p, end - p, "\"></div><div><label>Longitude</label>"
                      "<input name=lon%d inputmode=decimal value=\"", i);
        if (filled) {
            p = html_escape_append(p, end, cfg->locations[i].lon);
        }
        int show = filled ? cfg->show[i] : APP_SHOW_WEATHER;
        int km = filled ? cfg->radar_km[i] : APP_CONFIG_RADAR_KM_DEFAULT;
        int ship_km = filled ? cfg->ship_km[i] : APP_CONFIG_SHIP_KM_DEFAULT;
        p += snprintf(p, end - p, "\"></div></div>"
                      "<label>Show</label><div class=chk>"
                      "<label><input type=checkbox name=wx%d value=1%s>Weather</label>"
                      "<label><input type=checkbox name=ac%d value=1%s>Aircraft</label>"
                      "<label><input type=checkbox name=sh%d value=1%s>Ships</label></div>"
                      "<div class=row><div><label>Aircraft range (km)</label>"
                      "<input name=radarkm%d type=number inputmode=numeric min=%d max=%d value=%d></div>"
                      "<div><label>Ship range (km)</label>"
                      "<input name=shipkm%d type=number inputmode=numeric min=%d max=%d value=%d></div></div>"
                      "</fieldset>",
                      i, (show & APP_SHOW_WEATHER) ? " checked" : "",
                      i, (show & APP_SHOW_RADAR) ? " checked" : "",
                      i, (show & APP_SHOW_SHIPS) ? " checked" : "",
                      i, APP_CONFIG_RADAR_KM_MIN, APP_CONFIG_RADAR_KM_MAX, km,
                      i, APP_CONFIG_SHIP_KM_MIN, APP_CONFIG_SHIP_KM_MAX, ship_km);
    }

    p += snprintf(p, end - p, "%s", PAGE_TAIL);
    return buf;
}

/* --------------------------------------------------------------------------
 * HTTP handlers
 * ------------------------------------------------------------------------ */

static esp_err_t h_root(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_load(&cfg);
    char *page = build_page(&cfg);
    if (!page) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_sendstr(req, page);
    free(page);
    return err;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    httpd_resp_set_type(req, "application/json");

    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return httpd_resp_sendstr(req, "[]");
    }
    uint16_t n = PORTAL_MAX_SCAN;
    wifi_ap_record_t recs[PORTAL_MAX_SCAN];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        return httpd_resp_sendstr(req, "[]");
    }

    char out[1024];
    char *p = out;
    char *e = out + sizeof(out);
    p += snprintf(p, e - p, "[");
    int emitted = 0;
    for (int i = 0; i < n && p < e - 64; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)recs[i].ssid, (char *)recs[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        p += snprintf(p, e - p, "%s{\"s\":\"", emitted ? "," : "");
        p = html_escape_append(p, e, (char *)recs[i].ssid); /* also fine for JSON quoting of "&<> */
        p += snprintf(p, e - p, "\",\"r\":%d}", recs[i].rssi);
        emitted++;
    }
    snprintf(p, e - p, "]");
    return httpd_resp_sendstr(req, out);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

#define SAVE_BODY_MAX 4096

static esp_err_t save_form(httpd_req_t *req, const char *body);

static esp_err_t h_save(httpd_req_t *req)
{
    char *body = malloc(SAVE_BODY_MAX);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }
    int total = 0;
    while (total < req->content_len && total < SAVE_BODY_MAX - 1) {
        int r = httpd_req_recv(req, body + total, SAVE_BODY_MAX - 1 - total);
        if (r <= 0) {
            free(body);
            return httpd_resp_send_500(req);
        }
        total += r;
    }
    body[total] = '\0';
    esp_err_t err = save_form(req, body);
    free(body);
    return err;
}

static esp_err_t save_form(httpd_req_t *req, const char *body)
{

    app_config_t cfg;
    app_config_load(&cfg); /* keep the WiFi fields at their current value if omitted */
    form_field(body, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    form_field(body, "pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    char theme[4];
    if (form_field(body, "theme", theme, sizeof(theme))) {
        cfg.theme = (strcmp(theme, "1") == 0) ? APP_THEME_DARK : APP_THEME_LIGHT;
    }
    form_field(body, "aisid", cfg.ais_client_id, sizeof(cfg.ais_client_id));
    form_field(body, "aissec", cfg.ais_client_secret, sizeof(cfg.ais_client_secret));
    if (form_field(body, "yremail", cfg.yr_email, sizeof(cfg.yr_email)) &&
        !app_config_email_valid(cfg.yr_email)) {
        cfg.yr_email[0] = '\0'; /* blank or malformed: fall back to the default */
    }
    char minlen[8];
    if (form_field(body, "shipminlen", minlen, sizeof(minlen))) {
        long v = strtol(minlen, NULL, 10);
        cfg.ship_min_len_m = (v > 0 && v <= APP_CONFIG_SHIP_MIN_LEN_MAX) ? (uint16_t)v : 0;
    }

    httpd_resp_set_type(req, "text/html");
    if (cfg.wifi_ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            "<meta charset=utf-8><p>Invalid entry: a WiFi network is required."
            "<p><a href=/>Back</a>");
    }

    /* Collect the numbered location blocks (name0/lat0/lon0, ...). A block
     * with all three fields empty is skipped; the rest are compacted so the
     * stored list has no gaps. */
    app_location_t locs[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint8_t show[APP_CONFIG_MAX_LOCATIONS] = { 0 }; /* compacted like the locations */
    uint16_t radar_km[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    uint16_t ship_km[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    int n = 0;
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        char key[16];
        char name[APP_CONFIG_NAME_MAX];
        char lat[APP_CONFIG_COORD_MAX];
        char lon[APP_CONFIG_COORD_MAX];

        snprintf(key, sizeof(key), "name%d", i);
        form_field(body, key, name, sizeof(name));
        snprintf(key, sizeof(key), "lat%d", i);
        form_field(body, key, lat, sizeof(lat));
        snprintf(key, sizeof(key), "lon%d", i);
        form_field(body, key, lon, sizeof(lon));

        if (name[0] == '\0' && lat[0] == '\0' && lon[0] == '\0') {
            continue;
        }
        if (!app_config_coord_valid(lat, true) || !app_config_coord_valid(lon, false)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req,
                "<meta charset=utf-8><p>Invalid entry: every location needs a "
                "latitude within &plusmn;90 and a longitude within &plusmn;180."
                "<p><a href=/>Back</a>");
        }
        if (name[0] == '\0') {
            snprintf(name, sizeof(name), "Sted %d", n + 1);
        }
        snprintf(locs[n].name, sizeof(locs[n].name), "%s", name);
        snprintf(locs[n].lat, sizeof(locs[n].lat), "%s", lat);
        snprintf(locs[n].lon, sizeof(locs[n].lon), "%s", lon);

        /* Which screens (checkboxes: only ticked ones are sent) and the
         * ranges; nothing ticked or out of range falls back to weather / the
         * default range. */
        char val[8];
        uint8_t sh = 0;
        snprintf(key, sizeof(key), "wx%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_WEATHER : 0;
        snprintf(key, sizeof(key), "ac%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_RADAR : 0;
        snprintf(key, sizeof(key), "sh%d", i);
        sh |= form_field(body, key, val, sizeof(val)) ? APP_SHOW_SHIPS : 0;
        show[n] = sh ? sh : APP_SHOW_WEATHER;
        snprintf(key, sizeof(key), "radarkm%d", i);
        long km = form_field(body, key, val, sizeof(val)) ? strtol(val, NULL, 10) : 0;
        radar_km[n] = (km >= APP_CONFIG_RADAR_KM_MIN && km <= APP_CONFIG_RADAR_KM_MAX)
                          ? (uint16_t)km : APP_CONFIG_RADAR_KM_DEFAULT;
        snprintf(key, sizeof(key), "shipkm%d", i);
        km = form_field(body, key, val, sizeof(val)) ? strtol(val, NULL, 10) : 0;
        ship_km[n] = (km >= APP_CONFIG_SHIP_KM_MIN && km <= APP_CONFIG_SHIP_KM_MAX)
                         ? (uint16_t)km : APP_CONFIG_SHIP_KM_DEFAULT;
        n++;
    }

    if (n == 0) {
        /* Nothing entered - fall back to the compiled-in default location. */
        snprintf(locs[0].name, sizeof(locs[0].name), "%s", CONFIG_EXAMPLE_YR_LOCATION_NAME);
        snprintf(locs[0].lat, sizeof(locs[0].lat), "%s", CONFIG_EXAMPLE_YR_LATITUDE);
        snprintf(locs[0].lon, sizeof(locs[0].lon), "%s", CONFIG_EXAMPLE_YR_LONGITUDE);
        show[0] = APP_SHOW_WEATHER;
        radar_km[0] = APP_CONFIG_RADAR_KM_DEFAULT;
        ship_km[0] = APP_CONFIG_SHIP_KM_DEFAULT;
        n = 1;
    }

    memcpy(cfg.locations, locs, sizeof(cfg.locations));
    cfg.location_count = (uint8_t)n;
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        cfg.show[i] = show[i] ? show[i] : APP_SHOW_WEATHER;
        cfg.radar_km[i] = radar_km[i] ? radar_km[i] : APP_CONFIG_RADAR_KM_DEFAULT;
        cfg.ship_km[i] = ship_km[i] ? ship_km[i] : APP_CONFIG_SHIP_KM_DEFAULT;
    }

    if (app_config_save(&cfg) != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_sendstr(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<p style='font-family:system-ui;max-width:24rem;margin:3rem auto;text-align:center'>"
        "Saved. Restarting&hellip;</p>");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* Any other GET (captive-portal probes: /generate_204, /hotspot-detect.html,
 * ...) just gets the setup page. */
static esp_err_t h_catchall(httpd_req_t *req)
{
    return h_root(req);
}

static void start_web_server(void)
{
    if (s_httpd) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* h_save keeps the POST body (~1.6 KB) plus an app_config_t on its stack. */
    config.stack_size = 6144;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_httpd = NULL;
        return;
    }
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/save", .method = HTTP_POST, .handler = h_save });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/scan", .method = HTTP_GET, .handler = h_scan });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/", .method = HTTP_GET, .handler = h_root });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/*", .method = HTTP_GET, .handler = h_catchall });
}

esp_err_t wifi_provision_add_get_handler(const char *uri, esp_err_t (*handler)(httpd_req_t *req))
{
    if (s_httpd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Wildcard URIs match in registration order: move the catch-all last. */
    httpd_unregister_uri_handler(s_httpd, "/*", HTTP_GET);
    esp_err_t err = httpd_register_uri_handler(s_httpd,
                                               &(httpd_uri_t){ .uri = uri, .method = HTTP_GET, .handler = handler });
    httpd_register_uri_handler(s_httpd, &(httpd_uri_t){ .uri = "/*", .method = HTTP_GET, .handler = h_catchall });
    return err;
}

/* --------------------------------------------------------------------------
 * Captive-portal DNS: answer every A query with the portal IP
 * ------------------------------------------------------------------------ */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ip4_addr_t portal_ip;
    ip4addr_aton(PORTAL_AP_IP, &portal_ip);

    uint8_t pkt[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int len = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &flen);
        if (len < 12) {
            continue;
        }
        /* Turn the query into a response: QR=1, RA=1, one answer. */
        pkt[2] |= 0x80;
        pkt[3] = 0x80;
        pkt[6] = 0; pkt[7] = 1;   /* ANCOUNT = 1 */
        pkt[8] = 0; pkt[9] = 0;   /* NSCOUNT */
        pkt[10] = 0; pkt[11] = 0; /* ARCOUNT */
        if (len + 16 > (int)sizeof(pkt)) {
            continue;
        }
        uint8_t *a = pkt + len;
        *a++ = 0xC0; *a++ = 0x0C;              /* name -> pointer to the question */
        *a++ = 0x00; *a++ = 0x01;              /* type A */
        *a++ = 0x00; *a++ = 0x01;              /* class IN */
        *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x3C; /* TTL 60 */
        *a++ = 0x00; *a++ = 0x04;              /* RDLENGTH */
        memcpy(a, &portal_ip.addr, 4);
        a += 4;
        sendto(sock, pkt, a - pkt, 0, (struct sockaddr *)&from, flen);
    }
}

/* --------------------------------------------------------------------------
 * WiFi
 * ------------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_stop_reconnect) {
            wifi_event_sta_disconnected_t *e = data;
            s_retries++;
            ESP_LOGW(TAG, "WiFi disconnected (reason %d), retrying (attempt %d)...",
                     e ? e->reason : -1, s_retries);
            /* A short backoff before hammering esp_wifi_connect() again - most
             * useful right after power-on, when the AP itself may still be
             * booting (e.g. after a power outage) and every immediate retry
             * fails the same way for seconds at a time. Also keeps a fast
             * retry storm from adding to the DRAM pressure already tight at
             * boot (see the buffer_height comment in main.c). */
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

static bool boot_button_held(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    /* debounce: require it low for ~150 ms */
    for (int i = 0; i < 15; i++) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static void net_common_init(void)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "MultiDisplay-%02X%02X", mac[4], mac[5]);
}

static bool sta_try_connect(const app_config_t *cfg)
{
    s_stop_reconnect = false;
    s_retries = 0;
    xEventGroupClearBits(s_events, BIT_CONNECTED);

    wifi_config_t wc = { 0 }; /* zeroed, so a short copy is NUL-padded */
    memcpy(wc.sta.ssid, cfg->wifi_ssid,
           strnlen(cfg->wifi_ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, cfg->wifi_pass,
           strnlen(cfg->wifi_pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID '%s'...", cfg->wifi_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
    if (bits & BIT_CONNECTED) {
        return true;
    }
    s_stop_reconnect = true;
    esp_wifi_disconnect();
    return false;
}

static void portal_run(void)
{
    s_stop_reconnect = true;

    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); /* STA up too, so /scan works */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Setup portal: SSID '%s' -> http://%s/", s_ap_ssid, PORTAL_AP_IP);

    char msg[96];
    snprintf(msg, sizeof(msg), "Oppsett:\nKoble til WiFi \"%s\"\nog \xC3\xA5pne  http://%s", s_ap_ssid, PORTAL_AP_IP);
    status(msg);

    xTaskCreate(dns_task, "captdns", 3072, NULL, 4, NULL);
    start_web_server();

    /* Nothing more to do here - h_save reboots the device once the form is
     * submitted. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t wifi_provision_connect(const app_config_t *cfg, wifi_provision_status_fn status_fn)
{
    s_status = status_fn;

    app_config_t local = *cfg;
    net_common_init();

    bool force_portal = boot_button_held();
    if (force_portal) {
        ESP_LOGW(TAG, "BOOT held - forcing setup portal");
    }

    if (!force_portal && app_config_is_provisioned()) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Kobler til %s...", local.wifi_ssid);
        status(msg);
        if (sta_try_connect(&local)) {
            status("");
            start_web_server(); /* reachable on the station IP for later edits */
            return ESP_OK;
        }
        ESP_LOGW(TAG, "WiFi connect failed - opening setup portal");
        status("WiFi feilet \xE2\x80\x93 starter oppsett");
        esp_wifi_stop();
    }

    portal_run(); /* never returns */
    return ESP_OK;
}

const char *wifi_provision_get_ip(void)
{
    return s_sta_ip; /* "" until the station has an IP */
}
