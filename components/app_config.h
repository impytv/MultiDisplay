#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

#include <stdbool.h>
#include "esp_err.h"

/* Field sizes include room for the trailing NUL. WiFi SSID is 32 bytes,
 * password 64, per the 802.11 / WPA limits. */
#define APP_CONFIG_SSID_MAX  33
#define APP_CONFIG_PASS_MAX  65
#define APP_CONFIG_NAME_MAX  40
#define APP_CONFIG_COORD_MAX 16

typedef struct {
    char wifi_ssid[APP_CONFIG_SSID_MAX];
    char wifi_pass[APP_CONFIG_PASS_MAX];
    char loc_name[APP_CONFIG_NAME_MAX];
    char loc_lat[APP_CONFIG_COORD_MAX]; /* decimal degrees, as text (atof-ready) */
    char loc_lon[APP_CONFIG_COORD_MAX];
} app_config_t;

/**
 * Load the runtime configuration. Fields saved through the setup portal come
 * from NVS; anything never saved falls back to the compiled-in Kconfig
 * default (CONFIG_EXAMPLE_*). Always succeeds - a blank NVS just yields the
 * defaults.
 */
esp_err_t app_config_load(app_config_t *out);

/**
 * Persist the configuration to NVS and mark the device as provisioned.
 */
esp_err_t app_config_save(const app_config_t *cfg);

/**
 * True once app_config_save() has stored a WiFi SSID - i.e. the setup portal
 * has been completed at least once. False on a fresh device.
 */
bool app_config_is_provisioned(void);

/**
 * Forget the saved configuration. The next boot comes up in the setup
 * portal. (WiFi credentials the WiFi driver itself cached in NVS are not
 * touched here; wifi_provision always re-applies from app_config_t.)
 */
esp_err_t app_config_erase(void);

/**
 * Validate a latitude / longitude string: must parse fully as a number and
 * lie within [-90, 90] / [-180, 180].
 */
bool app_config_coord_valid(const char *text, bool is_latitude);

#endif
