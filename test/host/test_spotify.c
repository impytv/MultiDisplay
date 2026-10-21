#include "check.h"
#include "../../components/spotify_parse.c"

/* Replies shaped as Spotify's Web API reference gives them, cut down. */
static const char PLAYER[] =
    "{\"device\":{\"id\":\"abc123\",\"is_active\":true,\"is_private_session\":false,\"is_restricted\":false,"
    "\"name\":\"Stue\",\"type\":\"Speaker\",\"volume_percent\":45,\"supports_volume\":true},"
    "\"repeat_state\":\"off\",\"shuffle_state\":true,\"timestamp\":1760000000000,\"progress_ms\":61234,"
    "\"is_playing\":true,\"currently_playing_type\":\"track\","
    "\"item\":{\"type\":\"track\",\"id\":\"4uLU6hMCjMI75M1A2tKUQC\",\"name\":\"Kj\xC3\xA6rlighetsvise\","
    "\"duration_ms\":215000,\"artists\":[{\"name\":\"A\"},{\"name\":\"B \xC3\x98\"}],"
    "\"album\":{\"name\":\"Album\",\"images\":[{\"url\":\"https://i.scdn.co/640\",\"width\":640,\"height\":640},"
    "{\"url\":\"https://i.scdn.co/300\",\"width\":300,\"height\":300},"
    "{\"url\":\"https://i.scdn.co/64\",\"width\":64,\"height\":64}]}}}";

static const char EPISODE[] =
    "{\"actions\":{\"disallows\":{\"toggling_shuffle\":true,\"resuming\":true}},\"repeat_state\":\"track\","
    "\"device\":{\"id\":\"p1\",\"is_active\":true,\"name\":\"Telefon\",\"type\":\"Smartphone\","
    "\"volume_percent\":null,\"supports_volume\":false},\"is_playing\":false,\"progress_ms\":5,"
    "\"currently_playing_type\":\"episode\",\"item\":{\"type\":\"episode\",\"id\":\"ep\",\"name\":\"Episode 1\","
    "\"duration_ms\":3600000,\"images\":[{\"url\":\"https://i.scdn.co/ep640\",\"width\":640,\"height\":640}],"
    "\"show\":{\"name\":\"Podden\",\"publisher\":\"NRK\",\"images\":[]}}}";

static const char DEVICES[] =
    "{\"devices\":[{\"id\":\"d1\",\"is_active\":false,\"is_restricted\":false,\"name\":\"PC\",\"type\":\"Computer\","
    "\"volume_percent\":100,\"supports_volume\":true},{\"id\":null,\"name\":\"Uten id\",\"type\":\"TV\"},"
    "{\"id\":\"d3\",\"is_restricted\":true,\"name\":\"Bil\",\"type\":\"Automobile\"}]}";

/* Recently played, newest first: twice from a playlist, from an album,
 * from an artist (its album then), the same album again, from Liked Songs
 * and an old-style playlist URI. */
static const char RECENT[] =
    "{\"items\":["
    "{\"track\":{\"uri\":\"spotify:track:t1\",\"name\":\"T1\",\"album\":{\"uri\":\"spotify:album:a1\",\"name\":\"A1\","
    "\"images\":[{\"url\":\"https://i.scdn.co/a1\",\"width\":300}]}},"
    "\"context\":{\"type\":\"playlist\",\"uri\":\"spotify:playlist:p1\"}},"
    "{\"track\":{\"uri\":\"spotify:track:t2\",\"album\":{\"uri\":\"spotify:album:a2\",\"name\":\"A2\",\"images\":[]}},"
    "\"context\":{\"type\":\"playlist\",\"uri\":\"spotify:playlist:p1\"}},"
    "{\"track\":{\"uri\":\"spotify:track:t3\",\"album\":{\"uri\":\"spotify:album:a3\",\"name\":\"A3\",\"images\":[]}},"
    "\"context\":{\"type\":\"album\",\"uri\":\"spotify:album:a3\"}},"
    "{\"track\":{\"uri\":\"spotify:track:t4\",\"album\":{\"uri\":\"spotify:album:a4\",\"name\":\"A4\",\"images\":[]}},"
    "\"context\":{\"type\":\"artist\",\"uri\":\"spotify:artist:r4\"}},"
    "{\"track\":{\"uri\":\"spotify:track:t5\",\"album\":{\"uri\":\"spotify:album:a3\",\"name\":\"A3\",\"images\":[]}},"
    "\"context\":null},"
    "{\"track\":{\"uri\":\"spotify:track:t6\",\"album\":{\"uri\":\"spotify:album:a6\",\"name\":\"A6\",\"images\":[]}},"
    "\"context\":{\"type\":\"collection\",\"uri\":\"spotify:user:u:collection\"}},"
    "{\"track\":{\"uri\":\"spotify:local:x\",\"album\":{\"name\":\"Local\"}},\"context\":null},"
    "{\"track\":{\"uri\":\"spotify:track:t7\",\"album\":{\"uri\":\"spotify:album:a7\",\"name\":\"A7\"}},"
    "\"context\":{\"type\":\"playlist\",\"uri\":\"spotify:user:u:playlist:p7\"}}],"
    "\"next\":\"https://api.spotify.com/v1/me/player/recently-played?before=1760890000000&limit=10\","
    "\"cursors\":{\"after\":\"1760899999000\",\"before\":\"1760890000000\"}}";

/* The page before it, the last: the playlist again, and an album. */
static const char RECENT_LAST[] =
    "{\"items\":["
    "{\"track\":{\"uri\":\"spotify:track:t8\",\"album\":{\"uri\":\"spotify:album:a8\",\"name\":\"A8\"}},"
    "\"context\":{\"type\":\"playlist\",\"uri\":\"spotify:playlist:p1\"}},"
    "{\"track\":{\"uri\":\"spotify:track:t9\",\"album\":{\"uri\":\"spotify:album:a9\",\"name\":\"A9\"}},"
    "\"context\":{\"type\":\"album\",\"uri\":\"spotify:album:a9\"}}],"
    "\"next\":null,\"cursors\":{\"after\":\"1760889000000\",\"before\":\"1760880000000\"}}";

void test_spotify(void)
{
    spotify_player_t p;
    CHECK(spotify_parse_player(PLAYER, &p));
    CHECK(p.active && p.playing && p.shuffle);
    CHECK_STR(p.kind, "track");
    CHECK_STR(p.title, "Kj\xC3\xA6rlighetsvise");
    CHECK_STR(p.artist, "A, B \xC3\x98");
    CHECK_STR(p.album, "Album");
    CHECK_STR(p.image_url, "https://i.scdn.co/300"); /* the nearest 300 px */
    CHECK_INT(p.progress_ms, 61234);
    CHECK_INT(p.duration_ms, 215000);
    CHECK_STR(p.device.name, "Stue");
    CHECK(p.device.has_volume && p.device.volume == 45 && p.device.active);
    CHECK_INT(p.repeat, SPOTIFY_REPEAT_OFF);
    CHECK(p.can_shuffle && p.can_repeat && p.can_seek); /* nothing disallowed */

    CHECK(spotify_parse_player(EPISODE, &p));
    CHECK(!p.playing);
    CHECK_STR(p.artist, "Podden");
    CHECK_STR(p.album, "NRK");
    CHECK_STR(p.image_url, "https://i.scdn.co/ep640"); /* the episode's own; the show has none */
    CHECK(!p.device.has_volume && p.device.volume == -1);
    CHECK_INT(p.repeat, SPOTIFY_REPEAT_TRACK);
    CHECK(!p.can_shuffle && p.can_repeat && p.can_seek);
    CHECK(spotify_parse_player("{\"repeat_state\":\"context\",\"currently_playing_type\":\"ad\","
                               "\"actions\":{\"disallows\":{\"seeking\":false}},\"item\":{\"duration_ms\":30000}}",
                               &p));
    CHECK_INT(p.repeat, SPOTIFY_REPEAT_CONTEXT);
    CHECK(!p.can_seek); /* an ad: never */
    CHECK(!spotify_parse_player("<html>", &p));

    spotify_device_t d[SPOTIFY_DEVICES_MAX];
    CHECK_INT(spotify_parse_devices(DEVICES, d, SPOTIFY_DEVICES_MAX), 2); /* one without an id left out */
    CHECK_STR(d[0].name, "PC");
    CHECK(d[1].restricted);
    CHECK_INT(spotify_parse_devices(DEVICES, d, 1), 1);
    CHECK_INT(spotify_parse_devices("{}", d, 4), -1);

    char a[64], r[64];
    int exp = 0;
    CHECK(spotify_parse_token("{\"access_token\":\"AT\",\"token_type\":\"Bearer\",\"expires_in\":3600,"
                              "\"refresh_token\":\"RT\",\"scope\":\"x\"}",
                              a, sizeof(a), r, sizeof(r), &exp));
    CHECK_STR(a, "AT");
    CHECK_STR(r, "RT");
    CHECK_INT(exp, 3600);
    CHECK(spotify_parse_token("{\"access_token\":\"AT2\",\"expires_in\":60}", a, sizeof(a), r, sizeof(r), &exp));
    CHECK_STR(r, ""); /* no new refresh token: keep the old */
    CHECK(!spotify_parse_token("{\"error\":\"invalid_grant\"}", a, sizeof(a), r, sizeof(r), &exp));

    char e[64];
    CHECK(spotify_url_encode("a b/c~d:\xC3\xB8", e, sizeof(e)));
    CHECK_STR(e, "a%20b%2Fc~d%3A%C3%B8");
    CHECK(!spotify_url_encode("abcdef", e, 4));

    /* RFC 7636 appendix B: the verifier's SHA-256 in base64url. */
    static const uint8_t HASH[32] = { 19,  211, 30,  150, 26,  26,  216, 236, 47,  22,  177,
                                      12,  76,  152, 46,  8,   118, 168, 120, 173, 109, 241,
                                      68,  86,  110, 225, 137, 74,  203, 112, 249, 195 };
    char b64[48];
    spotify_base64url(HASH, sizeof(HASH), b64);
    CHECK_STR(b64, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    spotify_base64url((const uint8_t *)"ab", 2, b64);
    CHECK_STR(b64, "YWI");

    spotify_recent_t rec[SPOTIFY_RECENT_MAX];
    char before[32];
    CHECK_INT(spotify_parse_recent(RECENT, rec, 0, SPOTIFY_RECENT_MAX, before, sizeof(before)), 5);
    CHECK_STR(before, "1760890000000");
    CHECK_STR(rec[0].uri, "spotify:playlist:p1");
    CHECK_STR(rec[0].name, ""); /* named by spotify_get_playlist */
    CHECK_STR(rec[0].image_url, "https://i.scdn.co/a1");
    CHECK_STR(rec[1].uri, "spotify:album:a3");
    CHECK_STR(rec[1].name, "A3");
    CHECK_STR(rec[2].uri, "spotify:album:a4");
    CHECK_STR(rec[2].name, "A4");
    CHECK_STR(rec[3].uri, "spotify:album:a6");
    CHECK_STR(rec[4].uri, "spotify:playlist:p7");
    CHECK_INT(spotify_parse_recent(RECENT_LAST, rec, 5, SPOTIFY_RECENT_MAX, before, sizeof(before)), 6);
    CHECK_STR(rec[5].uri, "spotify:album:a9"); /* p1 was there already */
    CHECK_STR(before, "");                     /* the last page */
    CHECK_INT(spotify_parse_recent(RECENT, rec, 0, 2, before, sizeof(before)), 2);
    CHECK_INT(spotify_parse_recent(RECENT_LAST, rec, 2, 2, before, sizeof(before)), 2);
    CHECK_INT(spotify_parse_recent("{}", rec, 0, 2, before, sizeof(before)), -1);
    char pn[64], pi[160];
    CHECK(spotify_parse_playlist("{\"name\":\"Kos\",\"images\":[{\"url\":\"https://mosaic\",\"width\":null}]}", pn,
                                 sizeof(pn), pi, sizeof(pi)));
    CHECK_STR(pn, "Kos");
    CHECK_STR(pi, "https://mosaic");
    CHECK(!spotify_parse_playlist("{\"error\":{}}", pn, sizeof(pn), pi, sizeof(pi)));

    /* The account's playlists: one without an address, and one that isn't a
     * playlist, left out; null items too. */
    spotify_recent_t pl[4];
    CHECK_INT(spotify_parse_playlists(
                  "{\"items\":[{\"uri\":\"spotify:playlist:aa\",\"name\":\"Kos\",\"images\":[{\"url\":\"https://m\","
                  "\"width\":640},{\"url\":\"https://s\",\"width\":300}]},null,{\"name\":\"Uten\"},"
                  "{\"uri\":\"spotify:album:x\",\"name\":\"A\"},{\"uri\":\"spotify:playlist:bb\",\"name\":\"Trening\","
                  "\"images\":null},{\"uri\":\"spotify:playlist:cc\",\"name\":\"C\"}],\"next\":null}",
                  pl, 2),
              2);
    CHECK_STR(pl[0].uri, "spotify:playlist:aa");
    CHECK_STR(pl[0].name, "Kos");
    CHECK_STR(pl[0].image_url, "https://s");
    CHECK_STR(pl[1].uri, "spotify:playlist:bb");
    CHECK_STR(pl[1].image_url, "");
    CHECK_INT(spotify_parse_playlists("{}", pl, 2), -1);
    /* A mosaic's address, longer than most, kept whole; one too long left out. */
    char big[600];
    snprintf(big, sizeof(big), "{\"name\":\"M\",\"images\":[{\"url\":\"https://mosaic.scdn.co/300/%0160d\"}]}", 0);
    CHECK(spotify_parse_playlist(big, pn, sizeof(pn), pl[0].image_url, sizeof(pl[0].image_url)));
    CHECK_INT((int)strlen(pl[0].image_url), 187);
    CHECK(spotify_parse_playlist(big, pn, sizeof(pn), pi, sizeof(pi)));
    CHECK_STR(pi, "");

    /* The context played from, and "Kø". */
    CHECK(spotify_parse_player("{\"context\":{\"uri\":\"spotify:playlist:37i9dQZF1DX\"},\"is_playing\":true}", &p));
    CHECK_STR(p.context, "spotify:playlist:37i9dQZF1DX");
    CHECK(spotify_parse_player("{\"context\":{\"uri\":\"spotify:artist:0gxy\"}}", &p));
    CHECK_STR(p.context, ""); /* only a playlist or album is a tile */
    CHECK(spotify_parse_player(PLAYER, &p));
    CHECK_STR(p.context, "");
    spotify_track_t q[4];
    CHECK_INT(spotify_parse_queue("{\"currently_playing\":{\"name\":\"Now\",\"artists\":[{\"name\":\"A\"},{\"name\":\"B\"}]},"
                                  "\"queue\":[{\"name\":\"Next\",\"artists\":[{\"name\":\"C\"}]},"
                                  "{\"name\":\"Ep\",\"show\":{\"name\":\"Podden\"}},{\"name\":\"X\"},{\"name\":\"Y\"}]}",
                                  q, 4),
              4);
    CHECK_STR(q[0].title, "Now");
    CHECK_STR(q[0].artist, "A, B");
    CHECK_STR(q[1].artist, "C");
    CHECK_STR(q[2].artist, "Podden");
    CHECK_STR(q[3].title, "X"); /* no more than asked */
    CHECK_INT(spotify_parse_queue("{\"currently_playing\":null,\"queue\":[]}", q, 4), 0);
    CHECK_INT(spotify_parse_queue("{}", q, 4), -1);
}
