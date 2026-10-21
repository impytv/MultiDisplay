#ifndef _SONOS_CLIENT_H_
#define _SONOS_CLIENT_H_

/* Sonos speakers on the local network, through the UPnP control they answer
 * on port 1400 (as Home Assistant uses them; Sonos's cloud API isn't
 * needed, nor any login): the rooms, what plays in each, playback control
 * and the room's volume, the household's Sonos favourites, and starting a
 * favourite or something from Spotify.
 *
 * A room is a zone of the Sonos app (a stereo pair or a sub isn't a room
 * of its own). Rooms play together in a group, as the Sonos app groups
 * them: playback commands go to the group's coordinator, the volume is the
 * group's, and a room joins or leaves a group (sonos_join, sonos_leave).
 * Spotify items are queued the way the Sonos app shares them: Spotify's
 * URI in an "x-rincon-cpcontainer:" address, with Spotify's service in the
 * metadata - which needs Spotify added in the Sonos app. Sonos only takes
 * playlists, albums and tracks so. Sonos Radio's favourites (no address of
 * their own, r:type "shortcut") can't be started from outside the Sonos
 * app, nor through Sonos's own API, and are left out.
 *
 * The parsing (sonos_parse_*, sonos_spotify_share) is plain C, tested on
 * the host. */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SONOS_ROOMS_MAX 10
#define SONOS_FAVS_MAX  12
#define SONOS_TEXT_MAX  96
#define SONOS_URL_MAX   320

typedef struct {
    char name[48];  /* "Stua" */
    char uuid[32];  /* "RINCON_..." */
    char ip[16];
    char coord[32]; /* its group's coordinator: uuid if it leads, or plays alone */
} sonos_room_t;

typedef enum {
    SONOS_STOPPED,
    SONOS_PLAYING,
    SONOS_PAUSED,
    SONOS_TRANSITIONING,
} sonos_state_t;

typedef struct {
    sonos_state_t state;
    bool empty;         /* nothing to play: an empty queue, no source */
    bool stream;        /* radio and the like: no length, no skipping */
    char source[24];    /* "Spotify", "AirPlay", "Radio", "TV", "" */
    char station[SONOS_TEXT_MAX]; /* a stream's name, "" if not one or unknown */
    int radio;                    /* which of sonos_radios() plays, -1 = none */
    char title[SONOS_TEXT_MAX];
    char artist[SONOS_TEXT_MAX];
    char album[SONOS_TEXT_MAX];
    char art_url[SONOS_URL_MAX]; /* absolute, "" = none */
    int position_s, duration_s;  /* -1 if unknown */
    bool can_play, can_pause, can_next, can_previous;
    bool can_seek;
    bool queue;                  /* plays the room's queue: shuffle and repeat work */
    bool shuffle;                /* the queue's play mode, if `queue` */
    int repeat;                  /* SONOS_REPEAT_* */
    int volume;                  /* the room's, percent; -1 if unknown */
} sonos_now_t;

#define SONOS_REPEAT_OFF 0
#define SONOS_REPEAT_ALL 1
#define SONOS_REPEAT_ONE 2

typedef struct {
    char title[64];
    char uri[256];   /* to play: res, unescaped */
    char meta[1024]; /* its r:resMD, unescaped (DIDL-Lite) */
    char art_url[SONOS_URL_MAX];
} sonos_fav_t;

/* `in` with & < > " ' as entities into out; false if it doesn't fit. */
bool sonos_xml_escape(const char *in, char *out, size_t out_len);

/* The entities of `s` (&lt; &gt; &amp; &quot; &apos; &#N; &#xN;) as text,
 * in place. */
void sonos_xml_unescape(char *s);

/* The text of the first element named `tag` ("dc:title", "CurrentURI";
 * the prefix must match) in xml[0..len), unescaped once, into out (cut to
 * fit). False if there is none. */
bool sonos_xml_text(const char *xml, const char *tag, char *out, size_t out_len);

/* GetZoneGroupState's reply: the rooms, at most `max`, sorted by name; -1
 * if it isn't one. */
int sonos_parse_rooms(const char *reply, sonos_room_t *out, int max);

/* The rest of sonos_now_t from GetTransportInfo, GetPositionInfo,
 * GetMediaInfo and GetCurrentTransportActions' replies (any may be NULL);
 * relative cover addresses are made absolute with the speaker's `ip`. The
 * volume is left as it is. False if `transport` isn't a reply. */
bool sonos_parse_now(const char *transport, const char *position, const char *media, const char *actions,
                     const char *ip, sonos_now_t *out);

/* GetTransportSettings' play mode into *shuffle and *repeat; false if it
 * isn't a reply. */
bool sonos_parse_play_mode(const char *reply, bool *shuffle, int *repeat);

/* The play mode for SetPlayMode ("NORMAL", "SHUFFLE", ...). */
const char *sonos_play_mode(bool shuffle, int repeat);

/* Only the state from GetTransportInfo's reply; false if it isn't one. */
bool sonos_parse_state(const char *transport, sonos_state_t *out);

/* GetGroupVolume's (or GetVolume's) reply; -1 if it isn't one. */
int sonos_parse_volume(const char *reply);

/* A Browse of the favourites (FV:2): those that can be started, at most
 * `max`; -1 if it isn't one. */
int sonos_parse_favorites(const char *reply, const char *ip, sonos_fav_t *out, int max);

/* A SOAP fault's UPnP error code, 0 if `reply` isn't one. */
int sonos_parse_fault(const char *reply);

/* What to queue for Spotify's `spotify_uri` ("spotify:playlist:..",
 * "spotify:album:..", "spotify:track:.."): the address and its metadata
 * (DIDL-Lite, not yet escaped), with Spotify as Sonos service `service`
 * (2311 in most places, 3079 in some). False for anything else. */
bool sonos_spotify_share(const char *spotify_uri, int service, char *uri, size_t uri_len, char *meta,
                         size_t meta_len);

/* ---- On the display (the weather task) ---- */

/* The rooms: from the speaker that answered last, or else from the first
 * to answer an SSDP search. */
esp_err_t sonos_get_rooms(sonos_room_t *out, int max, int *count);

/* The rooms found last (any task): their names, at most `max`. */
int sonos_known_rooms(char (*names)[48], int max);

/* What plays in `room` (its group's coordinator), and the group's
 * volume. */
esp_err_t sonos_get_now(const sonos_room_t *room, sonos_now_t *out);

/* Only whether `room` (a coordinator) plays. */
esp_err_t sonos_get_state(const sonos_room_t *room, sonos_state_t *out);

/* `room` plays along with the group led by `coord_uuid`. */
esp_err_t sonos_join(const sonos_room_t *room, const char *coord_uuid);

/* `room` leaves its group, and stops; if it led, the others play on. */
esp_err_t sonos_leave(const sonos_room_t *room);

typedef enum {
    SONOS_PLAY,
    SONOS_PAUSE,
    SONOS_STOP, /* a radio, which can't pause */
    SONOS_NEXT,
    SONOS_PREVIOUS,
    SONOS_VOLUME, /* to `value` percent */
    SONOS_SEEK,   /* to `value` seconds into the track */
    SONOS_MODE,   /* the queue's play mode: shuffle if `value` & 1, repeat `value` >> 1 */
} sonos_cmd_t;

esp_err_t sonos_command(const sonos_room_t *room, sonos_cmd_t cmd, int value);

/* The household's favourites that can be started (asked of `room`'s
 * speaker), at most max. */
esp_err_t sonos_get_favorites(const sonos_room_t *room, sonos_fav_t *out, int max, int *count);

/* Start a favourite in `room`. */
esp_err_t sonos_play_favorite(const sonos_room_t *room, const sonos_fav_t *fav);

/* A radio station anyone can listen to, started as a plain stream: no
 * favourite or radio service needed in the Sonos app. */
typedef struct {
    const char *key;   /* "nrk_p1": kept by the setup page, names its logo */
    const char *name;  /* "NRK P1" */
    const char *short_name; /* "P1": on its tile without a logo */
    const char *url;   /* http://, an MP3 stream */
    bool nrk;          /* NRK's, else a commercial station */
} sonos_radio_t;

/* Norway's most listened to channels (NRK's, then the others by
 * popularity); their count into *count. */
const sonos_radio_t *sonos_radios(int *count);

/* Whether `key` is in `list`, keys separated by commas ("nrk_p1,p4"). */
bool sonos_radio_chosen(const char *list, const char *key);

/* The logo of radio `key` in the update site's bruk/radio/tiles.bin
 * (`blob`, `len` bytes; see scripts/build_radio_logos.py), a JPEG; false
 * if it has none. */
bool sonos_logo_find(const uint8_t *blob, size_t len, const char *key, const uint8_t **data, size_t *data_len);

/* What to play for `radio`: its address and metadata (DIDL-Lite, not yet
 * escaped), with `art` (its logo's address, "" = none) as the album art.
 * False if it doesn't fit. */
bool sonos_radio_share(const sonos_radio_t *radio, const char *art, char *uri, size_t uri_len, char *meta,
                       size_t meta_len);

/* Start `radio` in `room`, with its logo at `art` ("" = none). */
esp_err_t sonos_play_radio(const sonos_room_t *room, const sonos_radio_t *radio, const char *art);

/* Errors besides the transport's. */
#define SONOS_ERR_NO_SPOTIFY 0x7f30 /* Sonos took neither of Spotify's services: not added in the Sonos app */

/* Start Spotify's `spotify_uri` in `room`, replacing its queue. */
esp_err_t sonos_play_spotify(const sonos_room_t *room, const char *spotify_uri);

/* The picture at `url` (a cover, from a speaker or the web), in PSRAM for
 * the caller to free(). */
esp_err_t sonos_get_image(const char *url, unsigned char **data, size_t *len);

/* Give back the replies' buffer, on another screen. */
void sonos_client_release(void);

#endif
