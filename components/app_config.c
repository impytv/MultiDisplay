#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_config";

#define NVS_NS "multidisplay"

/* Overwrite dst with NVS string key `key` only if it is present and fits;
 * otherwise dst keeps whatever default was seeded into it. */
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len)
{
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len == 0 || len > dst_len) {
        return;
    }
    nvs_get_str(h, key, dst, &len);
}

/* Per-location APP_SHOW_* bits: the low byte of each under `key` (as
 * firmware from before APP_SHOW_SAT stored them all, and still reads), the
 * high byte under `key_hi`. false if `key` was never saved. */
static bool load_bits(nvs_handle_t h, const char *key, const char *key_hi, uint16_t *bits)
{
    uint8_t lo[APP_CONFIG_MAX_LOCATIONS], hi[APP_CONFIG_MAX_LOCATIONS] = { 0 };
    size_t len = sizeof(lo);
    if (nvs_get_blob(h, key, lo, &len) != ESP_OK || len != sizeof(lo)) {
        return false;
    }
    len = sizeof(hi);
    if (nvs_get_blob(h, key_hi, hi, &len) != ESP_OK || len != sizeof(hi)) {
        memset(hi, 0, sizeof(hi)); /* saved before APP_SHOW_SAT */
    }
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        bits[i] = (uint16_t)(lo[i] | (hi[i] << 8));
    }
    return true;
}

static esp_err_t save_bits(nvs_handle_t h, const char *key, const char *key_hi, const uint16_t *bits)
{
    uint8_t lo[APP_CONFIG_MAX_LOCATIONS], hi[APP_CONFIG_MAX_LOCATIONS];
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        lo[i] = (uint8_t)bits[i];
        hi[i] = (uint8_t)(bits[i] >> 8);
    }
    esp_err_t err = nvs_set_blob(h, key, lo, sizeof(lo));
    return err == ESP_OK ? nvs_set_blob(h, key_hi, hi, sizeof(hi)) : err;
}

/* Whatever was stored, leave every location with at least one screen and a
 * radar range inside the allowed span. */
static void sanitize_view_settings(app_config_t *c)
{
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        c->show[i] &= APP_SHOW_ALL;
        if (c->show[i] == 0) {
            c->show[i] = APP_SHOW_WEATHER;
        }
        if (c->radar_km[i] < APP_CONFIG_RADAR_KM_MIN || c->radar_km[i] > APP_CONFIG_RADAR_KM_MAX) {
            c->radar_km[i] = APP_CONFIG_RADAR_KM_DEFAULT;
        }
        if (c->ship_km[i] < APP_CONFIG_SHIP_KM_MIN || c->ship_km[i] > APP_CONFIG_SHIP_KM_MAX) {
            c->ship_km[i] = APP_CONFIG_SHIP_KM_DEFAULT;
        }
        if (c->rain_km[i] < APP_CONFIG_RAIN_KM_MIN || c->rain_km[i] > APP_CONFIG_RAIN_KM_MAX) {
            c->rain_km[i] = APP_CONFIG_RAIN_KM_DEFAULT;
        }
        if (c->ship_min_len_m[i] > APP_CONFIG_SHIP_MIN_LEN_MAX) {
            c->ship_min_len_m[i] = 0;
        }
        if (c->ship_near_km[i] > APP_CONFIG_SHIP_NEAR_KM_MAX) {
            c->ship_near_km[i] = 0;
        }
        if (c->ship_near_min_len_m[i] > APP_CONFIG_SHIP_MIN_LEN_MAX) {
            c->ship_near_min_len_m[i] = 0;
        }
    }
    if (c->theme != APP_THEME_DARK) {
        c->theme = APP_THEME_LIGHT;
    }
    c->dim_enabled = c->dim_enabled ? 1 : 0;
    c->night_off = c->night_off ? 1 : 0;
    if (c->dim_start >= 24 * 60 || c->dim_end >= 24 * 60) {
        c->dim_start = APP_CONFIG_DIM_START_DEFAULT;
        c->dim_end = APP_CONFIG_DIM_END_DEFAULT;
    }
    if (c->title_px < APP_CONFIG_TITLE_PX_MIN || c->title_px > APP_CONFIG_TITLE_PX_MAX) {
        c->title_px = APP_CONFIG_TITLE_PX_DEFAULT;
    }
    if (c->text_px < APP_CONFIG_TEXT_PX_MIN || c->text_px > APP_CONFIG_TEXT_PX_MAX) {
        c->text_px = APP_CONFIG_TEXT_PX_DEFAULT;
    }
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        c->auto_show[i] &= APP_SHOW_ALL;
    }
    if (c->auto_idle_min > APP_CONFIG_AUTO_IDLE_MIN_MAX) {
        c->auto_idle_min = 0;
    }
    if (c->auto_dwell_s < APP_CONFIG_AUTO_DWELL_S_MIN || c->auto_dwell_s > APP_CONFIG_AUTO_DWELL_S_MAX) {
        c->auto_dwell_s = APP_CONFIG_AUTO_DWELL_S_DEFAULT;
    }
    c->auto_overview = c->auto_overview ? 1 : 0;
    c->ov_show = c->ov_show ? 1 : 0;
    c->auto_night_pause = c->auto_night_pause ? 1 : 0;
    c->ota_auto = c->ota_auto ? 1 : 0;
    c->screen_ctl = c->screen_ctl ? 1 : 0;
    if (c->ota_at >= 24 * 60) {
        c->ota_at = APP_CONFIG_OTA_AT_DEFAULT;
    }
    if (c->ota_every_h < APP_CONFIG_OTA_EVERY_H_MIN || c->ota_every_h > APP_CONFIG_OTA_EVERY_H_MAX) {
        c->ota_every_h = APP_CONFIG_OTA_EVERY_H_DEFAULT;
    }
    char host[APP_CONFIG_DEVNAME_MAX];
    app_config_hostname(c->device_name, host, sizeof(host));
    snprintf(c->device_name, sizeof(c->device_name), "%s", host[0] ? host : APP_CONFIG_DEVNAME_DEFAULT);
    c->title_bold = c->title_bold ? 1 : 0;
    c->text_bold = c->text_bold ? 1 : 0;
    c->cal_show = c->cal_show ? 1 : 0;
    c->cal_rotate = c->cal_rotate ? 1 : 0;
    c->sat_show = c->sat_show ? 1 : 0;
    c->sat_rotate = c->sat_rotate ? 1 : 0;
    if (c->sat_zoom != 2 && c->sat_zoom != 3) {
        c->sat_zoom = APP_CONFIG_SAT_ZOOM_DEFAULT;
    }
    c->sat_visible = c->sat_visible ? 1 : 0;
    for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
        c->cal_url[i][APP_CONFIG_CAL_URL_MAX - 1] = '\0';
        if (!app_config_cal_url_valid(c->cal_url[i])) {
            c->cal_url[i][0] = '\0';
        }
    }
}

static void seed_defaults(app_config_t *out)
{
    snprintf(out->wifi_ssid, sizeof(out->wifi_ssid), "%s", CONFIG_EXAMPLE_WIFI_SSID);
    snprintf(out->wifi_pass, sizeof(out->wifi_pass), "%s", CONFIG_EXAMPLE_WIFI_PASSWORD);
    snprintf(out->locations[0].name, sizeof(out->locations[0].name), "%s", CONFIG_EXAMPLE_YR_LOCATION_NAME);
    snprintf(out->locations[0].lat, sizeof(out->locations[0].lat), "%s", CONFIG_EXAMPLE_YR_LATITUDE);
    snprintf(out->locations[0].lon, sizeof(out->locations[0].lon), "%s", CONFIG_EXAMPLE_YR_LONGITUDE);
    out->location_count = 1;
    out->dim_enabled = 1;
    out->dim_start = APP_CONFIG_DIM_START_DEFAULT;
    out->dim_end = APP_CONFIG_DIM_END_DEFAULT;
    out->title_px = APP_CONFIG_TITLE_PX_DEFAULT;
    out->text_px = APP_CONFIG_TEXT_PX_DEFAULT;
    out->auto_dwell_s = APP_CONFIG_AUTO_DWELL_S_DEFAULT;
    out->auto_night_pause = 1;
    out->ov_show = 1;
    out->sat_zoom = APP_CONFIG_SAT_ZOOM_DEFAULT;
    snprintf(out->ota_url, sizeof(out->ota_url), "%s", CONFIG_MULTIDISPLAY_OTA_DEFAULT_URL);
    out->ota_at = APP_CONFIG_OTA_AT_DEFAULT;
    out->ota_every_h = APP_CONFIG_OTA_EVERY_H_DEFAULT;
    snprintf(out->device_name, sizeof(out->device_name), "%s", APP_CONFIG_DEVNAME_DEFAULT);
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        out->show[i] = APP_SHOW_WEATHER;
        out->radar_km[i] = APP_CONFIG_RADAR_KM_DEFAULT;
        out->ship_km[i] = APP_CONFIG_SHIP_KM_DEFAULT;
        out->rain_km[i] = APP_CONFIG_RAIN_KM_DEFAULT;
    }
}

esp_err_t app_config_load(app_config_t *out)
{
    memset(out, 0, sizeof(*out));
    seed_defaults(out);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        /* Never opened / never written - everything is default. */
        return ESP_OK;
    }

    load_str(h, "ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    load_str(h, "pass", out->wifi_pass, sizeof(out->wifi_pass));

    size_t blob_len = sizeof(out->locations);
    if (nvs_get_blob(h, "locs", out->locations, &blob_len) == ESP_OK) {
        uint8_t cnt = 1;
        nvs_get_u8(h, "loccnt", &cnt);
        if (cnt < 1) {
            cnt = 1;
        } else if (cnt > APP_CONFIG_MAX_LOCATIONS) {
            cnt = APP_CONFIG_MAX_LOCATIONS;
        }
        out->location_count = cnt;
    } else {
        /* Legacy single-location layout (firmware before multi-location). */
        load_str(h, "name", out->locations[0].name, sizeof(out->locations[0].name));
        load_str(h, "lat", out->locations[0].lat, sizeof(out->locations[0].lat));
        load_str(h, "lon", out->locations[0].lon, sizeof(out->locations[0].lon));
        out->location_count = 1;
    }

    /* Per-location screens and radar ranges. Firmware before per-location
     * settings stored one radar bitmask ("radar": weather always, radar where
     * the bit is set) and one shared range ("radarkm"); read those if the new
     * keys aren't there yet. */
    if (!load_bits(h, "show", "showhi", out->show)) {
        uint8_t mask = 0;
        nvs_get_u8(h, "radar", &mask);
        for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
            out->show[i] = APP_SHOW_WEATHER | ((mask & (1u << i)) ? APP_SHOW_RADAR : 0);
        }
    }
    size_t len = sizeof(out->radar_km);
    if (nvs_get_blob(h, "radarkms", out->radar_km, &len) != ESP_OK || len != sizeof(out->radar_km)) {
        uint16_t km = APP_CONFIG_RADAR_KM_DEFAULT;
        nvs_get_u16(h, "radarkm", &km);
        for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
            out->radar_km[i] = km;
        }
    }
    len = sizeof(out->ship_km);
    if (nvs_get_blob(h, "shipkms", out->ship_km, &len) != ESP_OK || len != sizeof(out->ship_km)) {
        for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
            out->ship_km[i] = APP_CONFIG_SHIP_KM_DEFAULT;
        }
    }
    len = sizeof(out->rain_km);
    if (nvs_get_blob(h, "rainkms", out->rain_km, &len) != ESP_OK || len != sizeof(out->rain_km)) {
        for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
            out->rain_km[i] = APP_CONFIG_RAIN_KM_DEFAULT;
        }
    }
    len = sizeof(out->ship_min_len_m);
    if (nvs_get_blob(h, "shipminlens", out->ship_min_len_m, &len) != ESP_OK ||
        len != sizeof(out->ship_min_len_m)) {
        uint16_t m = 0; /* the older single setting for every location */
        nvs_get_u16(h, "shipminlen", &m);
        for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
            out->ship_min_len_m[i] = m;
        }
    }
    len = sizeof(out->ship_near_km); /* no inner circle if never saved */
    if (nvs_get_blob(h, "shipnearkms", out->ship_near_km, &len) != ESP_OK || len != sizeof(out->ship_near_km)) {
        memset(out->ship_near_km, 0, sizeof(out->ship_near_km));
    }
    len = sizeof(out->ship_near_min_len_m);
    if (nvs_get_blob(h, "shipnearlens", out->ship_near_min_len_m, &len) != ESP_OK ||
        len != sizeof(out->ship_near_min_len_m)) {
        memset(out->ship_near_min_len_m, 0, sizeof(out->ship_near_min_len_m));
    }
    if (!load_bits(h, "autoshow", "autoshowhi", out->auto_show)) {
        memset(out->auto_show, 0, sizeof(out->auto_show)); /* nothing in the rotation if never saved */
    }
    nvs_get_u16(h, "autoidle", &out->auto_idle_min); /* off if never saved */
    nvs_get_u16(h, "autodwell", &out->auto_dwell_s);
    nvs_get_u8(h, "autoov", &out->auto_overview);
    nvs_get_u8(h, "ovshow", &out->ov_show); /* keeps the default (shown) if never saved */
    nvs_get_u8(h, "autonight", &out->auto_night_pause); /* keeps the default (on) if never saved */
    len = sizeof(out->departures); /* no departure boards if never saved */
    if (nvs_get_blob(h, "deps", out->departures, &len) != ESP_OK || len != sizeof(out->departures)) {
        memset(out->departures, 0, sizeof(out->departures));
    }
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        out->departures[i][APP_CONFIG_DEPARTURES_MAX - 1] = '\0';
    }
    load_str(h, "aisid", out->ais_client_id, sizeof(out->ais_client_id));
    load_str(h, "aissec", out->ais_client_secret, sizeof(out->ais_client_secret));
    load_str(h, "yremail", out->yr_email, sizeof(out->yr_email));
    load_str(h, "webpass", out->web_pass, sizeof(out->web_pass));
    nvs_get_u8(h, "otaauto", &out->ota_auto); /* off if never saved */
    nvs_get_u16(h, "otaat", &out->ota_at);     /* 03:30 every 24 hours if never saved */
    nvs_get_u8(h, "otaevery", &out->ota_every_h);
    load_str(h, "otaurl", out->ota_url, sizeof(out->ota_url)); /* keeps the default if never saved */
    load_str(h, "devname", out->device_name, sizeof(out->device_name));
    nvs_get_u8(h, "scrctl", &out->screen_ctl); /* off if never saved */
    if (!app_config_email_valid(out->yr_email)) {
        out->yr_email[0] = '\0';
    }
    nvs_get_u8(h, "theme", &out->theme); /* leaves the light default if never saved */
    nvs_get_u8(h, "dimon", &out->dim_enabled); /* these three keep the defaults if never saved */
    nvs_get_u8(h, "nightoff", &out->night_off); /* dims if never saved */
    nvs_get_u16(h, "dimstart", &out->dim_start);
    nvs_get_u16(h, "dimend", &out->dim_end);
    nvs_get_u8(h, "titlepx", &out->title_px); /* the font settings too */
    nvs_get_u8(h, "titlebold", &out->title_bold);
    nvs_get_u8(h, "textpx", &out->text_px);
    nvs_get_u8(h, "textbold", &out->text_bold);
    nvs_get_u8(h, "calshow", &out->cal_show); /* no calendar if never saved */
    nvs_get_u8(h, "calrot", &out->cal_rotate);
    nvs_get_u8(h, "satshow", &out->sat_show); /* no satellite image if never saved */
    nvs_get_u8(h, "satrot", &out->sat_rotate);
    nvs_get_u8(h, "satzoom", &out->sat_zoom); /* 0 if never saved: the default, from sanitizing */
    nvs_get_u8(h, "satvis", &out->sat_visible);
    for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
        char key[] = "calurl0";
        key[6] = (char)('0' + i);
        load_str(h, key, out->cal_url[i], sizeof(out->cal_url[i]));
    }
    nvs_close(h);
    sanitize_view_settings(out);

    ESP_LOGI(TAG, "loaded: ssid='%s', %u location(s), first='%s' (%s, %s), %s theme, "
             "fonts %u px%s / %u px%s, BarentsWatch credentials %s, yr contact '%s', setup password %s",
             out->wifi_ssid, out->location_count, out->locations[0].name,
             out->locations[0].lat, out->locations[0].lon,
             out->theme == APP_THEME_DARK ? "dark" : "light",
             out->title_px, out->title_bold ? " bold" : "", out->text_px, out->text_bold ? " bold" : "",
             (out->ais_client_id[0] && out->ais_client_secret[0]) ? "set" : "missing",
             out->yr_email, out->web_pass[0] ? "set" : "none");
    ESP_LOGI(TAG, "overview %s; auto rotation: after %u min idle, %u s per screen, overview %s, %s at night",
             out->ov_show ? "shown" : "not shown", out->auto_idle_min, out->auto_dwell_s,
             out->auto_overview ? "included" : "not included",
             out->auto_night_pause ? "paused" : "running");
    ESP_LOGI(TAG, "firmware updates: %s from '%s'; name '%s'; screen control %s", out->ota_auto ? "automatic" : "manual",
             out->ota_url, out->device_name, out->screen_ctl ? "on" : "off");
    int feeds = 0;
    for (int i = 0; i < APP_CONFIG_CAL_FEEDS; i++) {
        feeds += out->cal_url[i][0] != '\0';
    }
    ESP_LOGI(TAG, "calendar: %s%s, %d calendar(s)", out->cal_show ? "shown" : "not shown",
             out->cal_rotate ? ", in the rotation" : "", feeds);
    ESP_LOGI(TAG, "satellite image of Europe: %s%s; close-ups x%u; %s", out->sat_show ? "shown" : "not shown",
             out->sat_rotate ? ", in the rotation" : "", out->sat_zoom,
             out->sat_visible ? "visible light by day" : "infrared always");
    for (int i = 0; i < out->location_count; i++) {
        ESP_LOGI(TAG, "  [%d] %s: show%s%s%s%s%s%s%s%s%s, radar range %u km, ship range %u km, ships from %u m "
                 "(%u m within %u km), rain range %u km, departures '%s', rotation 0x%03x",
                 i, out->locations[i].name,
                 (out->show[i] & APP_SHOW_WEATHER) ? " weather" : "",
                 (out->show[i] & APP_SHOW_RADAR) ? " radar" : "",
                 (out->show[i] & APP_SHOW_SHIPS) ? " ships" : "",
                 (out->show[i] & APP_SHOW_RAIN) ? " rain" : "",
                 (out->show[i] & APP_SHOW_DEPARTURES) ? " departures" : "",
                 (out->show[i] & APP_SHOW_WEEK) ? " week" : "",
                 (out->show[i] & APP_SHOW_AIR) ? " air" : "",
                 (out->show[i] & APP_SHOW_TIDE) ? " tide" : "",
                 (out->show[i] & APP_SHOW_SAT) ? " satellite" : "",
                 out->radar_km[i], out->ship_km[i], out->ship_min_len_m[i],
                 out->ship_near_min_len_m[i], out->ship_near_km[i], out->rain_km[i],
                 out->departures[i], out->auto_show[i]);
    }
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t cnt = cfg->location_count;
    if (cnt < 1) {
        cnt = 1;
    } else if (cnt > APP_CONFIG_MAX_LOCATIONS) {
        cnt = APP_CONFIG_MAX_LOCATIONS;
    }

    err = nvs_set_str(h, "ssid", cfg->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", cfg->wifi_pass);
    if (err == ESP_OK) err = nvs_set_blob(h, "locs", cfg->locations, sizeof(cfg->locations));
    if (err == ESP_OK) err = nvs_set_u8(h, "loccnt", cnt);
    if (err == ESP_OK) err = save_bits(h, "show", "showhi", cfg->show);
    if (err == ESP_OK) err = nvs_set_blob(h, "radarkms", cfg->radar_km, sizeof(cfg->radar_km));
    if (err == ESP_OK) err = nvs_set_blob(h, "shipkms", cfg->ship_km, sizeof(cfg->ship_km));
    if (err == ESP_OK) err = nvs_set_blob(h, "shipminlens", cfg->ship_min_len_m, sizeof(cfg->ship_min_len_m));
    if (err == ESP_OK) err = nvs_set_blob(h, "shipnearkms", cfg->ship_near_km, sizeof(cfg->ship_near_km));
    if (err == ESP_OK) err = nvs_set_blob(h, "shipnearlens", cfg->ship_near_min_len_m, sizeof(cfg->ship_near_min_len_m));
    if (err == ESP_OK) err = nvs_set_blob(h, "rainkms", cfg->rain_km, sizeof(cfg->rain_km));
    if (err == ESP_OK) err = nvs_set_blob(h, "deps", cfg->departures, sizeof(cfg->departures));
    if (err == ESP_OK) err = save_bits(h, "autoshow", "autoshowhi", cfg->auto_show);
    if (err == ESP_OK) err = nvs_set_u16(h, "autoidle", cfg->auto_idle_min);
    if (err == ESP_OK) err = nvs_set_u16(h, "autodwell", cfg->auto_dwell_s);
    if (err == ESP_OK) err = nvs_set_u8(h, "autoov", cfg->auto_overview ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "ovshow", cfg->ov_show ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "autonight", cfg->auto_night_pause ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, "aisid", cfg->ais_client_id);
    if (err == ESP_OK) err = nvs_set_str(h, "aissec", cfg->ais_client_secret);
    if (err == ESP_OK) err = nvs_set_str(h, "yremail", cfg->yr_email);
    if (err == ESP_OK) err = nvs_set_str(h, "webpass", cfg->web_pass);
    if (err == ESP_OK) err = nvs_set_u8(h, "otaauto", cfg->ota_auto ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u16(h, "otaat", cfg->ota_at);
    if (err == ESP_OK) err = nvs_set_u8(h, "otaevery", cfg->ota_every_h);
    if (err == ESP_OK) err = nvs_set_str(h, "otaurl", cfg->ota_url);
    if (err == ESP_OK) err = nvs_set_str(h, "devname", cfg->device_name);
    if (err == ESP_OK) err = nvs_set_u8(h, "scrctl", cfg->screen_ctl ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "theme", cfg->theme == APP_THEME_DARK ? APP_THEME_DARK : APP_THEME_LIGHT);
    if (err == ESP_OK) err = nvs_set_u8(h, "dimon", cfg->dim_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "nightoff", cfg->night_off ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u16(h, "dimstart", cfg->dim_start);
    if (err == ESP_OK) err = nvs_set_u16(h, "dimend", cfg->dim_end);
    if (err == ESP_OK) err = nvs_set_u8(h, "titlepx", cfg->title_px);
    if (err == ESP_OK) err = nvs_set_u8(h, "titlebold", cfg->title_bold ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "textpx", cfg->text_px);
    if (err == ESP_OK) err = nvs_set_u8(h, "textbold", cfg->text_bold ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "calshow", cfg->cal_show ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "calrot", cfg->cal_rotate ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "satshow", cfg->sat_show ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "satrot", cfg->sat_rotate ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(h, "satzoom", cfg->sat_zoom);
    if (err == ESP_OK) err = nvs_set_u8(h, "satvis", cfg->sat_visible ? 1 : 0);
    for (int i = 0; err == ESP_OK && i < APP_CONFIG_CAL_FEEDS; i++) {
        char key[] = "calurl0";
        key[6] = (char)('0' + i);
        err = nvs_set_str(h, key, cfg->cal_url[i]);
    }
    /* Retire the pre-per-location radar keys; missing keys just return NOT_FOUND. */
    nvs_erase_key(h, "radar");
    nvs_erase_key(h, "radarkm");
    nvs_erase_key(h, "shipminlen");
    if (err == ESP_OK) err = nvs_set_u8(h, "prov", 1);
    /* Retire the legacy single-location keys, if this NVS was written by an
     * older firmware. Missing keys just return NOT_FOUND - ignore. */
    nvs_erase_key(h, "name");
    nvs_erase_key(h, "lat");
    nvs_erase_key(h, "lon");
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved: ssid='%s', %u location(s), first='%s' (%s, %s)",
                 cfg->wifi_ssid, cnt, cfg->locations[0].name,
                 cfg->locations[0].lat, cfg->locations[0].lon);
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool app_config_is_provisioned(void)
{
    /* A build that already carries a real WiFi SSID in sdkconfig (not the
     * "myssid" placeholder) is considered set up - it connects straight away
     * and the web page is only for later edits. A fresh build with the
     * placeholder starts in the setup portal. Either way, a Save through the
     * portal takes over from then on. */
    if (strcmp(CONFIG_EXAMPLE_WIFI_SSID, "myssid") != 0) {
        return true;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t prov = 0;
    esp_err_t err = nvs_get_u8(h, "prov", &prov);
    nvs_close(h);
    return err == ESP_OK && prov != 0;
}

esp_err_t app_config_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGW(TAG, "configuration erased");
    return err;
}

void app_config_hostname(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    bool dash = false; /* a '-' is due before the next letter */
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < out_len; p++) {
        const char *add = NULL;
        char one[2] = { 0, 0 };
        if (*p >= 'A' && *p <= 'Z') {
            one[0] = (char)(*p - 'A' + 'a');
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) {
            one[0] = (char)*p;
        } else if (p[0] == 0xC3 && p[1] != 0) { /* æ ø å Æ Ø Å in UTF-8 */
            const unsigned char c = p[1] | 0x20;
            add = (c == 0xA6) ? "ae" : (c == 0xB8) ? "o" : (c == 0xA5) ? "aa" : NULL;
            p++;
        } else if (*p == ' ' || *p == '-' || *p == '_' || *p == '.') {
            dash = n > 0;
            continue;
        } else {
            continue; /* anything else: left out */
        }
        if (add == NULL) {
            add = one;
        }
        if (dash && n + 1 < out_len) {
            out[n++] = '-';
        }
        dash = false;
        for (; *add && n + 1 < out_len; add++) {
            out[n++] = *add;
        }
    }
    while (n > 0 && out[n - 1] == '-') {
        n--;
    }
    out[n] = '\0';
}

void app_config_sanitize(app_config_t *cfg)
{
    sanitize_view_settings(cfg);
}

bool app_config_coord_valid(const char *text, bool is_latitude)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    double v = strtod(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    double limit = is_latitude ? 90.0 : 180.0;
    return v >= -limit && v <= limit;
}

bool app_config_cal_url_valid(const char *text)
{
    if (text == NULL || (strncmp(text, "https://", 8) != 0 && strncmp(text, "http://", 7) != 0 &&
                         strncmp(text, "webcal://", 9) != 0)) {
        return false;
    }
    for (const char *c = text; *c; c++) {
        if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7f) {
            return false;
        }
    }
    return true;
}

bool app_config_email_valid(const char *text)
{
    if (text == NULL) {
        return false;
    }
    const char *at = strchr(text, '@');
    if (at == NULL || at == text || at[1] == '\0' || strchr(at + 1, '@') != NULL) {
        return false;
    }
    for (const char *c = text; *c; c++) {
        if ((unsigned char)*c <= ' ' || *c == 0x7f || *c == '(' || *c == ')') {
            return false;
        }
    }
    return true;
}

esp_err_t app_config_save_last_view(uint8_t view_index)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "view", view_index);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

uint8_t app_config_load_last_view(void)
{
    nvs_handle_t h;
    uint8_t view = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "view", &view); /* leaves `view` at 0 if never saved */
        nvs_close(h);
    }
    return view;
}
