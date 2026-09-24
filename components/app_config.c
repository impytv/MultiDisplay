#include <stdint.h>
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
    }
    if (c->ship_min_len_m > APP_CONFIG_SHIP_MIN_LEN_MAX) {
        c->ship_min_len_m = 0;
    }
    if (c->theme != APP_THEME_DARK) {
        c->theme = APP_THEME_LIGHT;
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
    for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
        out->show[i] = APP_SHOW_WEATHER;
        out->radar_km[i] = APP_CONFIG_RADAR_KM_DEFAULT;
        out->ship_km[i] = APP_CONFIG_SHIP_KM_DEFAULT;
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
    size_t len = sizeof(out->show);
    if (nvs_get_blob(h, "show", out->show, &len) != ESP_OK || len != sizeof(out->show)) {
        uint8_t mask = 0;
        nvs_get_u8(h, "radar", &mask);
        for (int i = 0; i < APP_CONFIG_MAX_LOCATIONS; i++) {
            out->show[i] = APP_SHOW_WEATHER | ((mask & (1u << i)) ? APP_SHOW_RADAR : 0);
        }
    }
    len = sizeof(out->radar_km);
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
    nvs_get_u16(h, "shipminlen", &out->ship_min_len_m);
    load_str(h, "aisid", out->ais_client_id, sizeof(out->ais_client_id));
    load_str(h, "aissec", out->ais_client_secret, sizeof(out->ais_client_secret));
    load_str(h, "yremail", out->yr_email, sizeof(out->yr_email));
    if (!app_config_email_valid(out->yr_email)) {
        out->yr_email[0] = '\0';
    }
    nvs_get_u8(h, "theme", &out->theme); /* leaves the light default if never saved */
    nvs_close(h);
    sanitize_view_settings(out);

    ESP_LOGI(TAG, "loaded: ssid='%s', %u location(s), first='%s' (%s, %s), %s theme, "
             "BarentsWatch credentials %s, ships from %u m, yr contact '%s'",
             out->wifi_ssid, out->location_count, out->locations[0].name,
             out->locations[0].lat, out->locations[0].lon,
             out->theme == APP_THEME_DARK ? "dark" : "light",
             (out->ais_client_id[0] && out->ais_client_secret[0]) ? "set" : "missing",
             out->ship_min_len_m, out->yr_email);
    for (int i = 0; i < out->location_count; i++) {
        ESP_LOGI(TAG, "  [%d] %s: show%s%s%s, radar range %u km, ship range %u km",
                 i, out->locations[i].name,
                 (out->show[i] & APP_SHOW_WEATHER) ? " weather" : "",
                 (out->show[i] & APP_SHOW_RADAR) ? " radar" : "",
                 (out->show[i] & APP_SHOW_SHIPS) ? " ships" : "",
                 out->radar_km[i], out->ship_km[i]);
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
    if (err == ESP_OK) err = nvs_set_blob(h, "show", cfg->show, sizeof(cfg->show));
    if (err == ESP_OK) err = nvs_set_blob(h, "radarkms", cfg->radar_km, sizeof(cfg->radar_km));
    if (err == ESP_OK) err = nvs_set_blob(h, "shipkms", cfg->ship_km, sizeof(cfg->ship_km));
    if (err == ESP_OK) err = nvs_set_u16(h, "shipminlen", cfg->ship_min_len_m);
    if (err == ESP_OK) err = nvs_set_str(h, "aisid", cfg->ais_client_id);
    if (err == ESP_OK) err = nvs_set_str(h, "aissec", cfg->ais_client_secret);
    if (err == ESP_OK) err = nvs_set_str(h, "yremail", cfg->yr_email);
    if (err == ESP_OK) err = nvs_set_u8(h, "theme", cfg->theme == APP_THEME_DARK ? APP_THEME_DARK : APP_THEME_LIGHT);
    /* Retire the pre-per-location radar keys; missing keys just return NOT_FOUND. */
    nvs_erase_key(h, "radar");
    nvs_erase_key(h, "radarkm");
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
