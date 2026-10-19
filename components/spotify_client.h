#ifndef _SPOTIFY_CLIENT_H_
#define _SPOTIFY_CLIENT_H_

/* Spotify's Web API (api.spotify.com): what is playing, and playback
 * control - play, pause, skipping, volume, and moving the playback to
 * another of the account's devices. Controlling needs Spotify Premium.
 *
 * Logging in (Authorization Code with PKCE, as for an app that can't keep a
 * secret): the setup page's link goes to /spotify/login on the display,
 * which sends the browser to Spotify with spotify_auth_url. Spotify only
 * returns to an https:// address, so it returns to a page on the update
 * site (server/spotify/), which sends the browser on to the display's
 * /spotify/svar with the code; spotify_auth_return keeps it, and the
 * weather task trades it for the tokens (spotify_auth_service). The
 * refresh token is kept in NVS (namespace "spotify"), for this display
 * only - not in backups.
 *
 * The parsing (spotify_parse_*) is plain C, tested on the host. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SPOTIFY_TEXT_MAX    96
#define SPOTIFY_DEVICES_MAX 8

typedef struct {
    char id[48];
    char name[48];
    char type[16];       /* "Computer", "Smartphone", "Speaker", ... */
    bool active;
    bool restricted;     /* takes no commands */
    bool has_volume;
    int volume;          /* percent; -1 if unknown */
} spotify_device_t;

typedef struct {
    bool active;         /* a device is playing or paused; false = nothing (HTTP 204) */
    bool playing;
    bool shuffle;
    char kind[12];       /* "track", "episode", "ad", "unknown" */
    char item_id[48];    /* tells one track from the next */
    char title[SPOTIFY_TEXT_MAX];
    char artist[SPOTIFY_TEXT_MAX]; /* the artists, or a podcast's show */
    char album[SPOTIFY_TEXT_MAX];
    char image_url[160]; /* the cover nearest SPOTIFY_COVER_PX wide, "" = none */
    int progress_ms, duration_ms;
    spotify_device_t device;
} spotify_player_t;

/* The cover size asked for (Spotify has 640, 300 and 64). */
#define SPOTIFY_COVER_PX 300

/* GET /v1/me/player's body into out; false if it isn't one. */
bool spotify_parse_player(const char *json, spotify_player_t *out);

/* GET /v1/me/player/devices' body: the devices, at most `max`; -1 if it
 * isn't one. */
int spotify_parse_devices(const char *json, spotify_device_t *out, int max);

/* A token reply: the access token, a new refresh token if one is given
 * ("" otherwise) and the access token's life in seconds. False if there is
 * no access token. */
bool spotify_parse_token(const char *json, char *access, size_t access_len, char *refresh, size_t refresh_len,
                         int *expires_in);

/* `in` percent-encoded for a URL or form (all but A-Z a-z 0-9 - . _ ~);
 * false if it doesn't fit. */
bool spotify_url_encode(const char *in, char *out, size_t out_len);

/* base64url without padding (RFC 7636); out needs 4 * ceil(len / 3) + 1. */
void spotify_base64url(const uint8_t *in, size_t len, char *out);

/* ---- On the display ---- */

/* Errors besides the transport's. */
#define SPOTIFY_ERR_LOGIN     0x7f20 /* not logged in, or the login was withdrawn */
#define SPOTIFY_ERR_PREMIUM   0x7f21 /* the account has no Premium: no control */
#define SPOTIFY_ERR_NO_DEVICE 0x7f22 /* nothing to send the command to */
#define SPOTIFY_ERR_RATE      0x7f23 /* asked too often: wait spotify_retry_after_s() */

typedef enum {
    SPOTIFY_NOT_CONNECTED,
    SPOTIFY_CONNECTING,   /* a code is waiting to be traded */
    SPOTIFY_CONNECTED,
    SPOTIFY_LOGIN_FAILED, /* the last attempt; see the log */
} spotify_login_t;

/* Load the login kept in NVS. `client_id` is the setup page's ("" = none:
 * nothing works), `redirect` app_config_spotify_redirect's. A login made
 * with another client ID is dropped. Before anything else here. */
void spotify_init(const char *client_id, const char *redirect);

/* The address to send the browser to for logging in, into out, with a new
 * verifier and state; `host` is the display as the browser reached it (the
 * Host header), where the update site's page sends the code. False with no
 * client ID or redirect address. Any task. */
bool spotify_auth_url(const char *host, char *out, size_t out_len);

/* Spotify's answer, as /spotify/svar got it: keeps the code for the weather
 * task if `state` is the one sent (any task). False otherwise, or with an
 * error (the login was refused). */
bool spotify_auth_return(const char *state, const char *code, const char *error);

/* Weather task, every pass: trade a waiting code for the tokens. */
void spotify_auth_service(void);

/* The login, with the account's name into user (may be NULL). Any task. */
spotify_login_t spotify_login_state(char *user, size_t user_len);

/* Forget the login. Any task. */
void spotify_disconnect(void);

/* What is playing. Weather task. */
esp_err_t spotify_get_player(spotify_player_t *out);

/* The account's devices, into out (at most max), the count into *count. */
esp_err_t spotify_get_devices(spotify_device_t *out, int max, int *count);

typedef enum {
    SPOTIFY_PLAY,
    SPOTIFY_PAUSE,
    SPOTIFY_NEXT,
    SPOTIFY_PREVIOUS,
    SPOTIFY_VOLUME,   /* to `value` percent */
    SPOTIFY_TRANSFER, /* playing on `device_id` */
} spotify_cmd_t;

esp_err_t spotify_command(spotify_cmd_t cmd, int value, const char *device_id);

/* The image at `url` (a cover), in PSRAM for the caller to free(). */
esp_err_t spotify_get_image(const char *url, uint8_t **data, size_t *len);

/* After SPOTIFY_ERR_RATE: seconds to wait. */
int spotify_retry_after_s(void);

/* Drop the kept-alive connection to api.spotify.com. */
void spotify_close(void);

#endif
