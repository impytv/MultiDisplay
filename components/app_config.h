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
#define APP_SHOW_DEPARTURES 0x10
#define APP_SHOW_AIR     0x20 /* air quality and pollen */
#define APP_SHOW_WEEK    0x40 /* the week ahead */
#define APP_SHOW_TIDE    0x80 /* tides and water level */
#define APP_SHOW_SAT     0x100 /* the satellite image around it */
#define APP_SHOW_ALL     (APP_SHOW_WEATHER | APP_SHOW_RADAR | APP_SHOW_SHIPS | APP_SHOW_RAIN | \
                          APP_SHOW_DEPARTURES | APP_SHOW_AIR | APP_SHOW_WEEK | APP_SHOW_TIDE | APP_SHOW_SAT)

/* The calendar screen (one for the display, not per location): up to this
 * many iCal addresses, merged. Secret, like a password: whoever has one can
 * read the calendar. */
#define APP_CONFIG_CAL_FEEDS   3
#define APP_CONFIG_CAL_URL_MAX 400

/* How much the satellite close-ups are blown up: 3 (about 2000 km across)
 * or 2 (about 3200 km, sharper). */
#define APP_CONFIG_SAT_ZOOM_DEFAULT 3

/* Public transport departures: which stops and lines a location shows, as
 * the text entur_parse_selection() reads (see entur_client.h). */
#define APP_CONFIG_DEPARTURES_MAX 200

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

/* Ship traffic: an optional inner circle (kilometres, 0 = none) with its own
 * minimum ship length, e.g. small boats close by but only big ships further
 * out. */
#define APP_CONFIG_SHIP_NEAR_KM_MAX APP_CONFIG_SHIP_KM_MAX

/* BarentsWatch API client credentials (https://www.barentswatch.no/minside/). */
#define APP_CONFIG_AIS_CRED_MAX 128

/* Automatic rotation: after auto_idle_min minutes without a touch (0 = never)
 * the display moves on to the next screen picked for it every auto_dwell_s
 * seconds, until the next touch. */
#define APP_CONFIG_AUTO_IDLE_MIN_MAX   240
#define APP_CONFIG_AUTO_DWELL_S_DEFAULT 30
#define APP_CONFIG_AUTO_DWELL_S_MIN    5
#define APP_CONFIG_AUTO_DWELL_S_MAX    3600

/* Contact email put in the User-Agent sent to api.met.no (yr). */
#define APP_CONFIG_EMAIL_MAX 64

/* The display's name on the network (app_config_t.device_name): its
 * <name>.local address and the host name the router sees. */
#define APP_CONFIG_DEVNAME_MAX     33
#define APP_CONFIG_DEVNAME_DEFAULT "multidisplay"

/* Firmware updates: the manifest URL the updater checks (see
 * main/updater.c). */
#define APP_CONFIG_OTA_URL_MAX 201
/* When to check for (and, if allowed, install) firmware updates: at
 * ota_at (local minutes after midnight), then every ota_every_h hours. */
#define APP_CONFIG_OTA_AT_DEFAULT      (3 * 60 + 30)
#define APP_CONFIG_OTA_EVERY_H_DEFAULT 24
#define APP_CONFIG_OTA_EVERY_H_MIN     1
#define APP_CONFIG_OTA_EVERY_H_MAX     168

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
     * APP_SHOW_RADAR, APP_SHOW_SHIPS, APP_SHOW_RAIN and APP_SHOW_DEPARTURES:
     * which screens location i gets, in that order. radar_km[i] / ship_km[i] / rain_km[i]
     * are the ranges of its aircraft radar, ship traffic and rain radar
     * screens; ship_min_len_m[i] hides its shorter ships (0 shows every
     * ship), except within ship_near_km[i] (0 = no inner circle), where
     * ship_near_min_len_m[i] applies instead. Bits from APP_SHOW_SAT up are
     * stored apart from the first eight (see app_config_load). */
    uint16_t show[APP_CONFIG_MAX_LOCATIONS];
    uint16_t radar_km[APP_CONFIG_MAX_LOCATIONS]; /* in [APP_CONFIG_RADAR_KM_MIN, APP_CONFIG_RADAR_KM_MAX] */
    uint16_t ship_km[APP_CONFIG_MAX_LOCATIONS];  /* in [APP_CONFIG_SHIP_KM_MIN, APP_CONFIG_SHIP_KM_MAX] */
    uint16_t ship_min_len_m[APP_CONFIG_MAX_LOCATIONS]; /* in [0, APP_CONFIG_SHIP_MIN_LEN_MAX] */
    uint16_t ship_near_km[APP_CONFIG_MAX_LOCATIONS];   /* in [0, APP_CONFIG_SHIP_NEAR_KM_MAX] */
    uint16_t ship_near_min_len_m[APP_CONFIG_MAX_LOCATIONS]; /* in [0, APP_CONFIG_SHIP_MIN_LEN_MAX] */
    uint16_t rain_km[APP_CONFIG_MAX_LOCATIONS];  /* in [APP_CONFIG_RAIN_KM_MIN, APP_CONFIG_RAIN_KM_MAX] */
    /* Where location i's ship traffic is centred, when not on the location
     * itself (e.g. out on the fjord rather than in town): a place name and
     * its coordinates, or all empty to use the location's own (see
     * app_config_ship_centre). */
    app_location_t ship_at[APP_CONFIG_MAX_LOCATIONS];
    /* Which of location i's screens the automatic rotation visits: APP_SHOW_*
     * bits, like show[i] (only screens that are shown count). */
    uint16_t auto_show[APP_CONFIG_MAX_LOCATIONS];
    /* The stops and lines of location i's departure board ("" = none). */
    char departures[APP_CONFIG_MAX_LOCATIONS][APP_CONFIG_DEPARTURES_MAX];
    uint8_t theme; /* APP_THEME_LIGHT or APP_THEME_DARK */
    /* Dim the screen from dim_start to dim_end local time (minutes after
     * midnight, each < 24 * 60; the window may wrap past midnight) when
     * dim_enabled. */
    uint8_t dim_enabled;
    /* During that window: 0 = dim the screen, 1 = switch it off (a touch
     * lights it for a minute). */
    uint8_t night_off;
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
    /* Automatic rotation (see APP_CONFIG_AUTO_*): minutes without a touch
     * before it starts (0 = off), seconds per screen, whether the overview
     * is one of its screens, and whether it pauses while the screen is
     * dimmed for the night (on unless unticked on the setup page). */
    uint16_t auto_idle_min;
    uint16_t auto_dwell_s;
    uint8_t auto_overview;
    uint8_t ov_show; /* the overview is one of the screens (on unless unticked) */
    uint8_t auto_night_pause;
    char ais_client_id[APP_CONFIG_AIS_CRED_MAX];
    char ais_client_secret[APP_CONFIG_AIS_CRED_MAX];
    /* Empty = use the compiled-in CONFIG_MULTIDISPLAY_USER_AGENT as is. */
    char yr_email[APP_CONFIG_EMAIL_MAX];
    /* The setup page's password; empty = none (see wifi_provision.c). */
    char web_pass[APP_CONFIG_PASS_MAX];
    /* Firmware updates: install new firmware from ota_url automatically
     * (off unless ticked on the setup page), at the scheduled checks: at
     * ota_at local time and every ota_every_h hours from then. ota_url is
     * also what "Check now" on the setup page checks. */
    uint8_t ota_auto;
    uint16_t ota_at;
    uint8_t ota_every_h;
    char ota_url[APP_CONFIG_OTA_URL_MAX];
    /* A valid host name (see app_config_hostname); per display, so not in
     * settings backups. */
    char device_name[APP_CONFIG_DEVNAME_MAX];
    /* Whether /screen may switch the screen on show, for screenshots of a
     * given screen (off unless ticked). Per display, so not in backups. */
    uint8_t screen_ctl;
    /* Whether the navigation page is served at / (on unless unticked). Per
     * display, so not in backups. */
    uint8_t nav_page;
    /* Whether a swipe up from the bottom edge of the display opens a menu
     * of the screens (on unless unticked). */
    uint8_t swipe_nav;
    /* The calendar screen: shown, in the rotation, and its calendars' iCal
     * addresses ("" = unused; see app_config_cal_url_valid). */
    uint8_t cal_show;
    uint8_t cal_rotate;
    char cal_url[APP_CONFIG_CAL_FEEDS][APP_CONFIG_CAL_URL_MAX];
    /* The satellite image of Europe: shown, and in the rotation. (The
     * close-ups around each location are APP_SHOW_SAT.) sat_zoom is how much
     * the close-ups are blown up (APP_CONFIG_SAT_ZOOM_*); with sat_visible
     * the visible-light image is shown instead of the infrared one while
     * it is light everywhere shown. */
    uint8_t sat_show;
    uint8_t sat_rotate;
    uint8_t sat_zoom;
    uint8_t sat_visible;
} app_config_t;

/**
 * Load the runtime configuration. Fields saved through the setup portal come
 * from NVS; anything never saved falls back to the compiled-in Kconfig
 * default (CONFIG_MULTIDISPLAY_*). Always succeeds - a blank NVS just
 * yields the defaults (a single location from the Kconfig coordinates).
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
 * `in` made into a host name in `out`: lower case, æ/ø/å as ae/o/aa, spaces
 * and other separators as '-', anything else left out, no '-' at either
 * end, at most out_len - 1 characters. "" if nothing usable is left.
 */
void app_config_hostname(const char *in, char *out, size_t out_len);

/**
 * Bring every setting into its allowed range (as app_config_load does).
 */
void app_config_sanitize(app_config_t *cfg);

/**
 * The settings as JSON, for a backup (caller frees). The WiFi network, its
 * password and the BarentsWatch secret are included only with `secrets`; the
 * setup password and the calendar addresses never are. NULL if out of memory.
 */
char *app_config_to_json(const app_config_t *cfg, bool secrets);

/**
 * Apply a backup made by app_config_to_json on top of `cfg` (normally the
 * current settings, whose WiFi and secrets are kept unless the backup has
 * them). Settings missing from the JSON keep their value. False, with a Norwegian reason in `err`,
 * if it isn't such a backup or a location is invalid; `cfg` is then
 * unchanged.
 */
bool app_config_from_json(const char *json, app_config_t *cfg, char *err, size_t err_len);

/**
 * Validate a latitude / longitude string: must parse fully as a number and
 * lie within [-90, 90] / [-180, 180].
 */
bool app_config_coord_valid(const char *text, bool is_latitude);

/**
 * Where location `i`'s ship traffic is centred: its own place for ships
 * (ship_at[i]) if one is set, else the location itself.
 */
const app_location_t *app_config_ship_centre(const app_config_t *cfg, int i);

/**
 * An iCal address the calendar screen can fetch: http://, https:// or
 * webcal:// (fetched as https://), with nothing that isn't printable ASCII.
 */
bool app_config_cal_url_valid(const char *text);

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
