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
    "{\"device\":{\"id\":\"p1\",\"is_active\":true,\"name\":\"Telefon\",\"type\":\"Smartphone\","
    "\"volume_percent\":null,\"supports_volume\":false},\"is_playing\":false,\"progress_ms\":5,"
    "\"currently_playing_type\":\"episode\",\"item\":{\"type\":\"episode\",\"id\":\"ep\",\"name\":\"Episode 1\","
    "\"duration_ms\":3600000,\"images\":[{\"url\":\"https://i.scdn.co/ep640\",\"width\":640,\"height\":640}],"
    "\"show\":{\"name\":\"Podden\",\"publisher\":\"NRK\",\"images\":[]}}}";

static const char DEVICES[] =
    "{\"devices\":[{\"id\":\"d1\",\"is_active\":false,\"is_restricted\":false,\"name\":\"PC\",\"type\":\"Computer\","
    "\"volume_percent\":100,\"supports_volume\":true},{\"id\":null,\"name\":\"Uten id\",\"type\":\"TV\"},"
    "{\"id\":\"d3\",\"is_restricted\":true,\"name\":\"Bil\",\"type\":\"Automobile\"}]}";

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

    CHECK(spotify_parse_player(EPISODE, &p));
    CHECK(!p.playing);
    CHECK_STR(p.artist, "Podden");
    CHECK_STR(p.album, "NRK");
    CHECK_STR(p.image_url, "https://i.scdn.co/ep640"); /* the episode's own; the show has none */
    CHECK(!p.device.has_volume && p.device.volume == -1);
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
}
