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

static void seed_defaults(app_config_t *out)
{
    snprintf(out->wifi_ssid, sizeof(out->wifi_ssid), "%s", CONFIG_EXAMPLE_WIFI_SSID);
    snprintf(out->wifi_pass, sizeof(out->wifi_pass), "%s", CONFIG_EXAMPLE_WIFI_PASSWORD);
    snprintf(out->locations[0].name, sizeof(out->locations[0].name), "%s", CONFIG_EXAMPLE_YR_LOCATION_NAME);
    snprintf(out->locations[0].lat, sizeof(out->locations[0].lat), "%s", CONFIG_EXAMPLE_YR_LATITUDE);
    snprintf(out->locations[0].lon, sizeof(out->locations[0].lon), "%s", CONFIG_EXAMPLE_YR_LONGITUDE);
    out->location_count = 1;
    out->radar_mask = 0;
    out->radar_range_km = APP_CONFIG_RADAR_KM_DEFAULT;
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

    nvs_get_u8(h, "radar", &out->radar_mask); /* absent (older firmware): stays 0 */
    out->radar_mask &= (uint8_t)((1u << out->location_count) - 1);
    uint16_t km = 0;
    if (nvs_get_u16(h, "radarkm", &km) == ESP_OK &&
        km >= APP_CONFIG_RADAR_KM_MIN && km <= APP_CONFIG_RADAR_KM_MAX) {
        out->radar_range_km = km;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "loaded: ssid='%s', %u location(s), first='%s' (%s, %s), radar mask 0x%02x range %u km",
             out->wifi_ssid, out->location_count, out->locations[0].name,
             out->locations[0].lat, out->locations[0].lon,
             out->radar_mask, out->radar_range_km);
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
    if (err == ESP_OK) err = nvs_set_u8(h, "radar", cfg->radar_mask & (uint8_t)((1u << cnt) - 1));
    if (err == ESP_OK) err = nvs_set_u16(h, "radarkm", cfg->radar_range_km);
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
