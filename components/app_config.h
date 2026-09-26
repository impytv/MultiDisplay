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

/* What a location shows: bit flags in app_config_t.show[]. */
#define APP_SHOW_WEATHER 0x01
#define APP_SHOW_RADAR   0x02
#define APP_SHOW_SHIPS   0x04
#define APP_SHOW_RAIN    0x08
#define APP_SHOW_ALL     (APP_SHOW_WEATHER | APP_SHOW_RADAR | APP_SHOW_SHIPS | APP_SHOW_RAIN)

/* Aircraft radar: how far out (kilometres) it looks around a location. */
#define APP_CONFIG_RADAR_KM_DEFAULT 40
#define APP_CONFIG_RADAR_KM_MIN     10
#define APP_CONFIG_RADAR_KM_MAX     185

/* Ship traffic: how far out (kilometres) it looks around a location. */
#define APP_CONFIG_SHIP_KM_DEFAULT 20
#define APP_CONFIG_SHIP_KM_MIN     2
#define APP_CONFIG_SHIP_KM_MAX     100

/* Rain radar: how far out (kilometres) it looks around a location. */
#define APP_CONFIG_RAIN_KM_DEFAULT 50
#define APP_CONFIG_RAIN_KM_MIN     10
#define APP_CONFIG_RAIN_KM_MAX     250

/* Ships shorter than this (metres) are not shown; 0 shows every ship. */
#define APP_CONFIG_SHIP_MIN_LEN_MAX 400

/* BarentsWatch API client credentials (https://www.barentswatch.no/minside/). */
#define APP_CONFIG_AIS_CRED_MAX 128

/* Contact email put in the User-Agent sent to api.met.no (yr). */
#define APP_CONFIG_EMAIL_MAX 64

/* Colour theme for every screen: app_config_t.theme. */
#define APP_THEME_LIGHT 0
#define APP_THEME_DARK  1

/* Night dimming (app_config_t.dim_*): the default window, in minutes after
 * local midnight. */
#define APP_CONFIG_DIM_START_DEFAULT (22 * 60)
#define APP_CONFIG_DIM_END_DEFAULT   (7 * 60)

/* Fonts (app_config_t.title_* / text_*): pixel sizes of the two text
 * classes. The screens are laid out at fixed positions, so the ranges stop
 * where text would start to run into its neighbours. */
#define APP_CONFIG_TITLE_PX_DEFAULT 25
#define APP_CONFIG_TITLE_PX_MIN     18
#define APP_CONFIG_TITLE_PX_MAX     36
#define APP_CONFIG_TEXT_PX_DEFAULT  17
#define APP_CONFIG_TEXT_PX_MIN      12
#define APP_CONFIG_TEXT_PX_MAX      21

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
    /* Per location, kept out of app_location_t so the stored "locs" blob
     * layout doesn't change. show[i] is any non-empty mix of APP_SHOW_WEATHER,
     * APP_SHOW_RADAR, APP_SHOW_SHIPS and APP_SHOW_RAIN: which screens
     * location i gets, in that order. radar_km[i] / ship_km[i] / rain_km[i]
     * are the ranges of its aircraft radar, ship traffic and rain radar
     * screens; ship_min_len_m[i] hides its shorter ships (0 shows every
     * ship). */
    uint8_t show[APP_CONFIG_MAX_LOCATIONS];
    uint16_t radar_km[APP_CONFIG_MAX_LOCATIONS]; /* in [APP_CONFIG_RADAR_KM_MIN, APP_CONFIG_RADAR_KM_MAX] */
    uint16_t ship_km[APP_CONFIG_MAX_LOCATIONS];  /* in [APP_CONFIG_SHIP_KM_MIN, APP_CONFIG_SHIP_KM_MAX] */
    uint16_t ship_min_len_m[APP_CONFIG_MAX_LOCATIONS]; /* in [0, APP_CONFIG_SHIP_MIN_LEN_MAX] */
    uint16_t rain_km[APP_CONFIG_MAX_LOCATIONS];  /* in [APP_CONFIG_RAIN_KM_MIN, APP_CONFIG_RAIN_KM_MAX] */
    uint8_t theme; /* APP_THEME_LIGHT or APP_THEME_DARK */
    /* Dim the screen from dim_start to dim_end local time (minutes after
     * midnight, each < 24 * 60; the window may wrap past midnight) when
     * dim_enabled. */
    uint8_t dim_enabled;
    uint16_t dim_start;
    uint16_t dim_end;
    /* Two font classes: title_* for the heading of each screen (the location
     * name, or the overview's title), text_* for everything else. *_px is the
     * size in pixels, within the APP_CONFIG_*_PX_MIN..MAX range; *_bold is 0
     * or 1. */
    uint8_t title_px;
    uint8_t title_bold;
    uint8_t text_px;
    uint8_t text_bold;
    char ais_client_id[APP_CONFIG_AIS_CRED_MAX];
    char ais_client_secret[APP_CONFIG_AIS_CRED_MAX];
    /* Empty = use the compiled-in CONFIG_EXAMPLE_YR_USER_AGENT as is. */
    char yr_email[APP_CONFIG_EMAIL_MAX];
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
 * Loose check of a contact email for the api.met.no User-Agent: one '@' with
 * text on both sides, and nothing that would break the header - no spaces,
 * control characters or parentheses.
 */
bool app_config_email_valid(const char *text);

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
