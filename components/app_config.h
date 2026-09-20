#ifndef _APP_CONFIG_H_
#define _APP_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Field sizes include room for the trailing NUL. WiFi SSID is 32 bytes,
 * password 64, per the 802.11 / WPA limits. */
#define APP_CONFIG_SSID_MAX  33
#define APP_CONFIG_PASS_MAX  65
#define APP_CONFIG_NAME_MAX  40
#define APP_CONFIG_COORD_MAX 16

/* How many forecast locations can be stored. The display shows one at a
 * time and cycles to the next when the screen is tapped. */
#define APP_CONFIG_MAX_LOCATIONS 5

/* Aircraft radar: how far out (kilometres) it looks around a location. */
#define APP_CONFIG_RADAR_KM_DEFAULT 40
#define APP_CONFIG_RADAR_KM_MIN     10
#define APP_CONFIG_RADAR_KM_MAX     185

typedef struct {
    char name[APP_CONFIG_NAME_MAX];
    char lat[APP_CONFIG_COORD_MAX]; /* decimal degrees, as text (atof-ready) */
    char lon[APP_CONFIG_COORD_MAX];
} app_location_t;

typedef struct {
    char wifi_ssid[APP_CONFIG_SSID_MAX];
    char wifi_pass[APP_CONFIG_PASS_MAX];
    app_location_t locations[APP_CONFIG_MAX_LOCATIONS];
    uint8_t location_count; /* always in [1, APP_CONFIG_MAX_LOCATIONS] */
    /* Bit i set = location i also gets an aircraft-radar screen after its
     * weather screen. Kept out of app_location_t so the stored "locs" blob
     * layout doesn't change. */
    uint8_t radar_mask;
    uint16_t radar_range_km; /* in [APP_CONFIG_RADAR_KM_MIN, APP_CONFIG_RADAR_KM_MAX] */
} app_config_t;

/**
 * Load the runtime configuration. Fields saved through the setup portal come
 * from NVS; anything never saved falls back to the compiled-in Kconfig
 * default (CONFIG_EXAMPLE_*). Always succeeds - a blank NVS just yields the
 * defaults (a single location from the Kconfig coordinates).
 */
esp_err_t app_config_load(app_config_t *out);

/**
 * Persist the configuration to NVS and mark the device as provisioned.
 * location_count is clamped into [1, APP_CONFIG_MAX_LOCATIONS].
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

/**
 * Remember which screen was on display (0 = the locations overview, 1..N =
 * that location's detail screen) so a reboot of any kind - the nightly
 * maintenance restart, a power cycle, a crash - comes back up showing the
 * same thing instead of always starting over at the overview.
 */
esp_err_t app_config_save_last_view(uint8_t view_index);

/**
 * The screen index last saved by app_config_save_last_view(), or 0 (the
 * overview) if none was ever saved.
 */
uint8_t app_config_load_last_view(void);

#endif
