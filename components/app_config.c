#include <stdlib.h>
#include <string.h>

#include "app_config.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_config";

#define NVS_NS "multidisplay"

/* Copy an NVS string key into dst, or fall back to `dflt` if the key is
 * absent. dst is always NUL-terminated. */
static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len,
                     const char *dflt)
{
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err != ESP_OK) {
        snprintf(dst, dst_len, "%s", dflt);
    }
}

esp_err_t app_config_load(app_config_t *out)
{
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* Never opened / never written - everything is default. */
        snprintf(out->wifi_ssid, sizeof(out->wifi_ssid), "%s", CONFIG_EXAMPLE_WIFI_SSID);
        snprintf(out->wifi_pass, sizeof(out->wifi_pass), "%s", CONFIG_EXAMPLE_WIFI_PASSWORD);
        snprintf(out->loc_name, sizeof(out->loc_name), "%s", CONFIG_EXAMPLE_YR_LOCATION_NAME);
        snprintf(out->loc_lat, sizeof(out->loc_lat), "%s", CONFIG_EXAMPLE_YR_LATITUDE);
        snprintf(out->loc_lon, sizeof(out->loc_lon), "%s", CONFIG_EXAMPLE_YR_LONGITUDE);
        return ESP_OK;
    }

    load_str(h, "ssid", out->wifi_ssid, sizeof(out->wifi_ssid), CONFIG_EXAMPLE_WIFI_SSID);
    load_str(h, "pass", out->wifi_pass, sizeof(out->wifi_pass), CONFIG_EXAMPLE_WIFI_PASSWORD);
    load_str(h, "name", out->loc_name, sizeof(out->loc_name), CONFIG_EXAMPLE_YR_LOCATION_NAME);
    load_str(h, "lat", out->loc_lat, sizeof(out->loc_lat), CONFIG_EXAMPLE_YR_LATITUDE);
    load_str(h, "lon", out->loc_lon, sizeof(out->loc_lon), CONFIG_EXAMPLE_YR_LONGITUDE);
    nvs_close(h);

    ESP_LOGI(TAG, "loaded: ssid='%s' loc='%s' (%s, %s)",
             out->wifi_ssid, out->loc_name, out->loc_lat, out->loc_lon);
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, "ssid", cfg->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", cfg->wifi_pass);
    if (err == ESP_OK) err = nvs_set_str(h, "name", cfg->loc_name);
    if (err == ESP_OK) err = nvs_set_str(h, "lat", cfg->loc_lat);
    if (err == ESP_OK) err = nvs_set_str(h, "lon", cfg->loc_lon);
    if (err == ESP_OK) err = nvs_set_u8(h, "prov", 1);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved: ssid='%s' loc='%s' (%s, %s)",
                 cfg->wifi_ssid, cfg->loc_name, cfg->loc_lat, cfg->loc_lon);
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
