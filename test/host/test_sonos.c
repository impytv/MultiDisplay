#include <stdlib.h>
#include "check.h"
#include "../../components/sonos_parse.c"
#include "../../components/app_config.h"

/* Replies from a household's speakers (data/sonos/): a stereo pair "Stua"
 * playing AirPlay, "Soverom" and "Badet" alone; NRK P3 and an album from
 * Spotify playing in Soverom. */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    b[fread(b, 1, n, f)] = '\0';
    fclose(f);
    return b;
}

void test_sonos(void)
{
    char buf[128];
    CHECK(sonos_xml_escape("a<b & \"c\"", buf, sizeof(buf)));
    CHECK_STR(buf, "a&lt;b &amp; &quot;c&quot;");
    CHECK(!sonos_xml_escape("<<<<", buf, 8));
    snprintf(buf, sizeof(buf), "&lt;x&gt; &amp;amp; &#248;&#xE6; &bogus; &#;");
    sonos_xml_unescape(buf);
    CHECK_STR(buf, "<x> &amp; \xC3\xB8\xC3\xA6 &bogus; &#;");
    CHECK(sonos_xml_text("<a><b x=\"1\">t&amp;t</b><bb>no</bb></a>", "b", buf, sizeof(buf)));
    CHECK_STR(buf, "t&t");
    CHECK(sonos_xml_text("<a><b/><b>2</b></a>", "b", buf, sizeof(buf)));
    CHECK_STR(buf, "");
    CHECK(!sonos_xml_text("<a><bb>1</bb></a>", "b", buf, sizeof(buf)));

    /* The rooms: the pair once, by its name; sorted. Each leads itself. */
    char *zgs = slurp("data/sonos/zgs.xml");
    sonos_room_t rooms[SONOS_ROOMS_MAX];
    CHECK_INT(sonos_parse_rooms(zgs, rooms, SONOS_ROOMS_MAX), 3);
    CHECK_STR(rooms[0].name, "Badet");
    CHECK_STR(rooms[0].ip, "192.168.0.45");
    CHECK_STR(rooms[1].name, "Soverom");
    CHECK_STR(rooms[1].uuid, "RINCON_542A1B54E9E801400");
    CHECK_STR(rooms[2].name, "Stua");
    CHECK_STR(rooms[2].ip, "192.168.0.61");
    CHECK_STR(rooms[2].coord, "RINCON_804AF2C8CCCE01400");
    CHECK_STR(rooms[2].uuid, rooms[2].coord);
    CHECK_INT(sonos_parse_rooms(zgs, rooms, 2), 2);
    CHECK_INT(sonos_parse_rooms("<x/>", rooms, 2), -1);
    free(zgs);
    /* A group of two rooms. */
    const char *grouped =
        "<s:Envelope><s:Body><u:GetZoneGroupStateResponse><ZoneGroupState>&lt;ZoneGroupState&gt;&lt;ZoneGroups&gt;"
        "&lt;ZoneGroup Coordinator=&quot;RINCON_B&quot; ID=&quot;x&quot;&gt;"
        "&lt;ZoneGroupMember UUID=&quot;RINCON_A&quot; Location=&quot;http://10.0.0.2:1400/x.xml&quot; "
        "ZoneName=&quot;K&amp;amp;B&quot;/&gt;"
        "&lt;ZoneGroupMember UUID=&quot;RINCON_B&quot; Location=&quot;http://10.0.0.3:1400/x.xml&quot; "
        "ZoneName=&quot;Stue&quot;&gt;&lt;Satellite UUID=&quot;RINCON_S&quot; ZoneName=&quot;Stue&quot;/&gt;"
        "&lt;/ZoneGroupMember&gt;&lt;/ZoneGroup&gt;"
        "&lt;ZoneGroup Coordinator=&quot;RINCON_Z&quot; ID=&quot;y&quot;&gt;&lt;ZoneGroupMember "
        "UUID=&quot;RINCON_Z&quot; Location=&quot;http://10.0.0.9:1400/x.xml&quot; ZoneName=&quot;Boost&quot; "
        "IsZoneBridge=&quot;1&quot;/&gt;&lt;/ZoneGroup&gt;"
        "&lt;/ZoneGroups&gt;&lt;/ZoneGroupState&gt;</ZoneGroupState></u:GetZoneGroupStateResponse></s:Body>"
        "</s:Envelope>";
    CHECK_INT(sonos_parse_rooms(grouped, rooms, SONOS_ROOMS_MAX), 2);
    CHECK_STR(rooms[0].name, "K&B");
    CHECK_STR(rooms[0].ip, "10.0.0.2");
    CHECK_STR(rooms[0].coord, "RINCON_B"); /* plays along with Stue */
    CHECK_STR(rooms[1].name, "Stue");
    CHECK_STR(rooms[1].uuid, "RINCON_B");
    CHECK_STR(rooms[1].coord, "RINCON_B");

    /* AirPlay, paused. */
    char *tr = slurp("data/sonos/transport.xml"), *pos = slurp("data/sonos/pos_airplay.xml");
    char *media = slurp("data/sonos/media_airplay.xml"), *act = slurp("data/sonos/actions.xml");
    sonos_now_t now = { .volume = 14 };
    CHECK(sonos_parse_now(tr, pos, media, act, "192.168.0.61", &now));
    CHECK_INT(now.state, SONOS_PAUSED);
    CHECK_STR(now.source, "AirPlay");
    CHECK_STR(now.title, "Victory Lap Five");
    CHECK(strncmp(now.artist, "Fred again.., Skepta", 20) == 0);
    CHECK_STR(now.album, "Victory Lap");
    CHECK_STR(now.art_url, "http://192.168.0.61:1400/getaa?v=0&vli=1&u=185860913");
    CHECK_INT(now.position_s, 129);
    CHECK_INT(now.duration_s, 345);
    CHECK(now.can_play && now.can_pause && now.can_next && now.can_previous && !now.stream && !now.empty);
    CHECK(!now.queue && !now.can_seek); /* AirPlay: no shuffle, no winding */
    CHECK_INT(now.volume, 14);
    free(tr);
    free(pos);
    free(media);
    free(act);
    CHECK(!sonos_parse_now("<x/>", NULL, NULL, NULL, "1.2.3.4", &now));
    sonos_state_t st;
    CHECK(sonos_parse_state("<CurrentTransportState>PAUSED_PLAYBACK</CurrentTransportState>", &st));
    CHECK_INT(st, SONOS_PAUSED);
    CHECK(!sonos_parse_state("<x/>", &st));

    /* NRK P3 from a favourite: the station's name and logo, no pause. */
    tr = slurp("data/sonos/transport_playing.xml");
    pos = slurp("data/sonos/pos_radio.xml");
    media = slurp("data/sonos/media_radio.xml");
    act = slurp("data/sonos/actions_radio.xml");
    CHECK(sonos_parse_now(tr, pos, media, act, "192.168.0.123", &now));
    CHECK_INT(now.state, SONOS_PLAYING);
    CHECK(now.stream);
    CHECK_STR(now.source, "Radio");
    CHECK_STR(now.title, "NRK P3 Musikk");
    CHECK_STR(now.artist, "");
    CHECK_STR(now.art_url, "https://gfx.nrk.no/F4h3WlHK4YUujwYdbFsM5QORv-xqxofMsvznIYSiBGsw");
    CHECK_INT(now.duration_s, -1);
    CHECK(now.can_play && !now.can_pause && !now.can_next && !now.can_previous);
    CHECK(!now.queue && !now.can_seek);
    free(pos);
    free(act);
    /* What the station says it plays, Sonos's way and as fields. */
    pos = "<TrackMetaData>&lt;DIDL-Lite&gt;&lt;item&gt;&lt;r:streamContent&gt;Kygo - Firestone&lt;/r:streamContent&gt;"
          "&lt;upnp:class&gt;object.item&lt;/upnp:class&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</TrackMetaData>";
    CHECK(sonos_parse_now(tr, pos, media, NULL, "1.2.3.4", &now));
    CHECK_STR(now.title, "Kygo - Firestone");
    CHECK_STR(now.artist, "NRK P3 Musikk");
    pos = "<TrackMetaData>&lt;DIDL-Lite&gt;&lt;item&gt;&lt;r:streamContent&gt;TYPE=SNG|TITLE Firestone|ARTIST "
          "Kygo|ALBUM &lt;/r:streamContent&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</TrackMetaData>";
    CHECK(sonos_parse_now(tr, pos, media, NULL, "1.2.3.4", &now));
    CHECK_STR(now.title, "Kygo - Firestone");
    /* A long title cut to whole characters, the artist still there. */
    char longpos[512], want[96];
    snprintf(longpos, sizeof(longpos), "<TrackMetaData>&lt;DIDL-Lite&gt;&lt;item&gt;&lt;r:streamContent&gt;TYPE=SNG|TITLE ");
    for (int i = 0; i < 50; i++) {
        strcat(longpos, "\xC3\xA6");
    }
    strcat(longpos, "|ARTIST Kygo|ALBUM &lt;/r:streamContent&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</TrackMetaData>");
    CHECK(sonos_parse_now(tr, longpos, media, NULL, "1.2.3.4", &now));
    snprintf(want, sizeof(want), "Kygo - ");
    for (int i = 0; i < 22; i++) {
        strcat(want, "\xC3\xA6");
    }
    CHECK_STR(now.title, want);
    free(media);

    /* An album shared from Spotify, from the queue. */
    pos = slurp("data/sonos/pos_spotify.xml");
    media = slurp("data/sonos/media_spotify.xml");
    CHECK(sonos_parse_now(tr, pos, media, NULL, "192.168.0.123", &now));
    CHECK_STR(now.source, "Spotify");
    CHECK_STR(now.title, "Never Gonna Give You Up");
    CHECK_STR(now.artist, "Rick Astley");
    CHECK_STR(now.album, "Whenever You Need Somebody");
    CHECK(strncmp(now.art_url, "http://192.168.0.123:1400/getaa?s=1&u=x-sonos-spotify%3aspotify%253atrack", 72) == 0);
    CHECK_INT(now.duration_s, 213);
    CHECK(now.can_next);
    CHECK(now.queue && now.can_seek); /* the queue: shuffle, repeat and winding */
    CHECK(sonos_parse_now(tr, pos, media, "<Actions>Set, Stop, Pause, Play, X_DLNA_SeekTime, Next</Actions>",
                          "192.168.0.123", &now));
    CHECK(now.can_seek && !now.can_previous);
    free(tr);
    free(pos);
    free(media);
    /* Stopped with an empty queue. */
    CHECK(sonos_parse_now("<CurrentTransportState>STOPPED</CurrentTransportState>",
                          "<TrackDuration>0:00:00</TrackDuration><TrackMetaData></TrackMetaData><TrackURI></TrackURI>",
                          "<CurrentURI>x-rincon-queue:RINCON_X#0</CurrentURI>", NULL, "1.2.3.4", &now));
    CHECK(now.empty);
    CHECK_INT(now.duration_s, -1);
    CHECK(now.queue && !now.can_seek); /* nothing to wind */

    /* The queue's play mode, both ways. */
    bool shuffle = false;
    int repeat = -1;
    CHECK(sonos_parse_play_mode("<PlayMode>SHUFFLE</PlayMode><RecQualityMode>X</RecQualityMode>", &shuffle, &repeat));
    CHECK(shuffle);
    CHECK_INT(repeat, SONOS_REPEAT_ALL);
    CHECK(sonos_parse_play_mode("<PlayMode>REPEAT_ONE</PlayMode>", &shuffle, &repeat));
    CHECK(!shuffle);
    CHECK_INT(repeat, SONOS_REPEAT_ONE);
    CHECK(sonos_parse_play_mode("<PlayMode>NEW_MODE</PlayMode>", &shuffle, &repeat)); /* unknown: plain */
    CHECK(!shuffle && repeat == SONOS_REPEAT_OFF);
    CHECK(!sonos_parse_play_mode("<x/>", &shuffle, &repeat));
    CHECK_STR(sonos_play_mode(false, SONOS_REPEAT_OFF), "NORMAL");
    CHECK_STR(sonos_play_mode(true, SONOS_REPEAT_OFF), "SHUFFLE_NOREPEAT");
    CHECK_STR(sonos_play_mode(true, SONOS_REPEAT_ONE), "SHUFFLE_REPEAT_ONE");
    CHECK_STR(sonos_play_mode(false, SONOS_REPEAT_ALL), "REPEAT_ALL");
    CHECK_STR(sonos_play_mode(false, 7), "NORMAL");
    for (int sh = 0; sh < 2; sh++) {
        for (int r = 0; r < 3; r++) {
            char m[64];
            snprintf(m, sizeof(m), "<PlayMode>%s</PlayMode>", sonos_play_mode(sh, r));
            CHECK(sonos_parse_play_mode(m, &shuffle, &repeat) && shuffle == sh && repeat == r);
        }
    }

    /* NRK's channels, as plain streams. */
    int nradio = 0;
    const sonos_radio_t *radios = sonos_radios(&nradio);
    CHECK_INT(nradio, 28);
    /* Each key once, each with its logo in the update site's tiles.bin. */
    FILE *lf = fopen("../../server/bruk/radio/tiles.bin", "rb");
    CHECK(lf != NULL);
    static uint8_t logos[400000];
    const size_t nlogos = lf ? fread(logos, 1, sizeof(logos), lf) : 0;
    if (lf) {
        fclose(lf);
    }
    CHECK(nlogos > 8 && nlogos < sizeof(logos));
    for (int i = 0; i < nradio; i++) {
        const uint8_t *pic = NULL;
        size_t plen = 0;
        CHECK(sonos_logo_find(logos, nlogos, radios[i].key, &pic, &plen));
        CHECK(plen > 1000 && pic[0] == 0xFF && pic[1] == 0xD8); /* a JPEG */
        for (int k = 0; k < i; k++) {
            CHECK(strcmp(radios[i].key, radios[k].key) != 0);
        }
    }
    const uint8_t *pic = NULL;
    size_t plen = 0;
    CHECK(!sonos_logo_find(logos, nlogos, "nope", &pic, &plen));
    CHECK(!sonos_logo_find(logos, 100, "p4", &pic, &plen)); /* cut short */
    CHECK(!sonos_logo_find((const uint8_t *)"XXXX\1\0\0\0", 8, "p4", &pic, &plen));
    /* The chosen ones: whole keys only. */
    CHECK(sonos_radio_chosen("nrk_p1,p4", "p4"));
    CHECK(sonos_radio_chosen("nrk_p1,p4", "nrk_p1"));
    CHECK(!sonos_radio_chosen("nrk_p1,p4", "p"));
    CHECK(!sonos_radio_chosen("nrk_p1,p4", "nrk_p1pluss"));
    CHECK(!sonos_radio_chosen("nrk_p1pluss", "nrk_p1"));
    CHECK(!sonos_radio_chosen("", "p4"));
    CHECK(!sonos_radio_chosen("p4", ""));
    /* The setup page's default: real keys, no more than the tiles. */
    int defaults = 0;
    for (int i = 0; i < nradio; i++) {
        defaults += sonos_radio_chosen(APP_CONFIG_SONOS_RADIOS_DEFAULT, radios[i].key);
    }
    int keys = 1;
    for (const char *c = APP_CONFIG_SONOS_RADIOS_DEFAULT; *c; c++) {
        keys += *c == ',';
    }
    CHECK_INT(defaults, keys);
    CHECK(defaults <= APP_CONFIG_SONOS_RADIOS_SHOWN);
    char ruri[160], rmeta[1024];
    CHECK(sonos_radio_share(&radios[0], "", ruri, sizeof(ruri), rmeta, sizeof(rmeta)));
    CHECK_STR(ruri, "x-rincon-mp3radio://cdn0-47115-liveicecast0.dna.contentdelivery.net/p1_mp3_h");
    CHECK(strstr(rmeta, "<dc:title>NRK P1</dc:title>") != NULL);
    CHECK(strstr(rmeta, "object.item.audioItem.audioBroadcast") != NULL);
    for (int i = 0; i < nradio; i++) {
        CHECK(sonos_radio_share(&radios[i], "", ruri, sizeof(ruri), rmeta, sizeof(rmeta)));
        CHECK(strlen(radios[i].short_name) < 24); /* a tile's label */
    }
    CHECK(!sonos_radio_share(&radios[0], "", ruri, 30, rmeta, sizeof(rmeta)));
    const sonos_radio_t https = { "x", "X", "X", "https://example.com/x", false };
    CHECK(!sonos_radio_share(&https, "", ruri, sizeof(ruri), rmeta, sizeof(rmeta)));
    /* Playing one: a radio, named and pictured from the metadata sent. */
    CHECK(sonos_radio_share(&radios[0], "https://example.no/bruk/radio/nrk_p1.jpg?a&b", ruri, sizeof(ruri), rmeta,
                            sizeof(rmeta)));
    CHECK(strstr(rmeta, "<upnp:albumArtURI>https://example.no/bruk/radio/nrk_p1.jpg?a&amp;b</upnp:albumArtURI>"));
    char media_r[1600], esc[1200];
    CHECK(sonos_xml_escape(rmeta, esc, sizeof(esc)));
    snprintf(media_r, sizeof(media_r), "<CurrentURI>%s</CurrentURI><CurrentURIMetaData>%s</CurrentURIMetaData>", ruri,
             esc);
    CHECK(sonos_parse_now("<CurrentTransportState>PLAYING</CurrentTransportState>",
                          "<TrackDuration>0:00:00</TrackDuration><TrackMetaData></TrackMetaData>", media_r, NULL,
                          "1.2.3.4", &now));
    CHECK(now.stream && !now.queue && !now.can_seek);
    CHECK_INT(now.radio, 0); /* NRK P1, from its address */
    CHECK_STR(now.art_url, "https://example.no/bruk/radio/nrk_p1.jpg?a&b");
    CHECK_STR(now.source, "Radio");
    CHECK_STR(now.title, "NRK P1");
    /* As Sonos says it: the title is the address's file name, and the
     * programme comes as streamContent. */
    CHECK(sonos_parse_now("<CurrentTransportState>PLAYING</CurrentTransportState>",
                          "<TrackDuration>0:00:00</TrackDuration><TrackURI>x-rincon-mp3radio://http://"
                          "cdn0-47115-liveicecast0.dna.contentdelivery.net/p1_mp3_h</TrackURI><TrackMetaData>"
                          "&lt;DIDL-Lite&gt;&lt;item&gt;&lt;r:streamContent&gt;Nyhetsmorgen&lt;/r:streamContent&gt;"
                          "&lt;dc:title&gt;p1_mp3_h&lt;/dc:title&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</TrackMetaData>",
                          media_r, NULL, "1.2.3.4", &now));
    CHECK_STR(now.title, "Nyhetsmorgen");
    CHECK_STR(now.artist, "NRK P1");
    CHECK_STR(now.station, "NRK P1");
    /* While it starts: Sonos' word, in Norwegian. */
    const char *const zp[][2] = { { "ZPSTR_BUFFERING", "Laster \xE2\x80\xA6" },
                                  { "ZPSTR_CONNECTING", "Kobler til \xE2\x80\xA6" },
                                  { "ZPSTR_SOMETHING_ELSE", "NRK P1" } };
    for (size_t i = 0; i < sizeof(zp) / sizeof(zp[0]); i++) {
        char pos[512];
        snprintf(pos, sizeof(pos),
                 "<TrackURI>x-rincon-mp3radio://http://h/p1_mp3_h</TrackURI><TrackMetaData>&lt;DIDL-Lite&gt;"
                 "&lt;item&gt;&lt;r:streamContent&gt;%s&lt;/r:streamContent&gt;&lt;dc:title&gt;p1_mp3_h"
                 "&lt;/dc:title&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</TrackMetaData>",
                 zp[i][0]);
        CHECK(sonos_parse_now("<CurrentTransportState>TRANSITIONING</CurrentTransportState>", pos, media_r, NULL,
                              "1.2.3.4", &now));
        CHECK_STR(now.title, zp[i][1]);
    }
    CHECK_STR(now.station, "NRK P1");
    /* No programme: the station's name. */
    CHECK(sonos_parse_now("<CurrentTransportState>PLAYING</CurrentTransportState>",
                          "<TrackURI>x-rincon-mp3radio://http://h/p1_mp3_h</TrackURI><TrackMetaData>"
                          "&lt;DIDL-Lite&gt;&lt;item&gt;&lt;dc:title&gt;p1_mp3_h&lt;/dc:title&gt;"
                          "&lt;/item&gt;&lt;/DIDL-Lite&gt;</TrackMetaData>",
                          media_r, NULL, "1.2.3.4", &now));
    CHECK_STR(now.title, "NRK P1");
    /* Started without its metadata: named from the address. */
    CHECK(sonos_parse_now("<CurrentTransportState>PLAYING</CurrentTransportState>",
                          "<TrackURI>x-rincon-mp3radio://http://cdn0-47115-liveicecast0.dna.contentdelivery.net/"
                          "nyheter_mp3_h</TrackURI><TrackMetaData>&lt;DIDL-Lite&gt;&lt;item&gt;&lt;dc:title&gt;"
                          "nyheter_mp3_h&lt;/dc:title&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</TrackMetaData>",
                          "<CurrentURI>x-rincon-mp3radio://cdn0-47115-liveicecast0.dna.contentdelivery.net/"
                          "nyheter_mp3_h</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>",
                          NULL, "1.2.3.4", &now));
    CHECK_STR(now.title, "NRK Nyheter");
    CHECK(now.radio >= 0 && strcmp(radios[now.radio].key, "nrk_nyheter") == 0);

    char *v = slurp("data/sonos/volume.xml");
    CHECK_INT(sonos_parse_volume(v), 14);
    CHECK_INT(sonos_parse_volume("<x/>"), -1);
    free(v);

    /* Favourites: of the three, only NRK P3 can be started (the two Sonos
     * Radio ones have no address). */
    char *fav = slurp("data/sonos/fav.xml");
    static sonos_fav_t favs[SONOS_FAVS_MAX];
    CHECK_INT(sonos_parse_favorites(fav, "192.168.0.61", favs, SONOS_FAVS_MAX), 1);
    CHECK_STR(favs[0].title, "NRK P3 Musikk");
    CHECK_STR(favs[0].uri, "x-sonosapi-stream:live%3Ap3musikk?sid=277&flags=8292&sn=4");
    CHECK(strncmp(favs[0].meta, "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\"", 54) == 0);
    CHECK(strstr(favs[0].meta, "<dc:title>NRK P3 Musikk</dc:title>") != NULL);
    CHECK_STR(favs[0].art_url, "https://gfx.nrk.no/F4h3WlHK4YUujwYdbFsM5QORv-xqxofMsvznIYSiBGsw");
    CHECK_INT(sonos_parse_favorites("<x/>", "1.2.3.4", favs, 2), -1);
    free(fav);
    /* An address too long to keep is left out, not cut: 60 "&" are 300
     * characters escaped. */
    char longfav[1024];
    for (int k = 0; k < 2; k++) {
        snprintf(longfav, sizeof(longfav), "<Result>&lt;DIDL-Lite&gt;&lt;item id=&quot;1&quot;&gt;&lt;dc:title&gt;Lang"
                                           "&lt;/dc:title&gt;&lt;res&gt;");
        for (int i = 0; i < (k == 0 ? 60 : 40); i++) {
            strcat(longfav, "&amp;amp;");
        }
        strcat(longfav, "&lt;/res&gt;&lt;r:resMD&gt;x&lt;/r:resMD&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</Result>");
        CHECK_INT(sonos_parse_favorites(longfav, "1.2.3.4", favs, 2), k == 0 ? 0 : 1);
    }
    CHECK_INT((int)strlen(favs[0].uri), 40);

    char *fault = slurp("data/sonos/fault800.xml");
    CHECK_INT(sonos_parse_fault(fault), 800);
    CHECK_INT(sonos_parse_fault("<errorCode>800</errorCode>"), 0); /* not a fault */
    free(fault);

    /* Spotify shared to Sonos. */
    char uri[160], meta[1024];
    CHECK(sonos_spotify_share("spotify:playlist:37i9dQZF1DXcBWIGoYBM5M", 2311, uri, sizeof(uri), meta, sizeof(meta)));
    CHECK_STR(uri, "x-rincon-cpcontainer:1006206cspotify%3aplaylist%3a37i9dQZF1DXcBWIGoYBM5M");
    CHECK(strstr(meta, "<item id=\"1006206cspotify%3aplaylist%3a37i9dQZF1DXcBWIGoYBM5M\" parentID=\"-1\"") != NULL);
    CHECK(strstr(meta, "<upnp:class>object.container.playlistContainer</upnp:class>") != NULL);
    CHECK(strstr(meta, ">SA_RINCON2311_X_#Svc2311-0-Token</desc>") != NULL);
    CHECK(sonos_spotify_share("spotify:album:6N9PS4QXF1D0OWPk0Sxtb4", 3079, uri, sizeof(uri), meta, sizeof(meta)));
    CHECK_STR(uri, "x-rincon-cpcontainer:1004206cspotify%3aalbum%3a6N9PS4QXF1D0OWPk0Sxtb4");
    CHECK(strstr(meta, "SA_RINCON3079_X_#Svc3079-0-Token") != NULL);
    CHECK(sonos_spotify_share("spotify:track:4cOdK2wGLETKBW3PvgPWqT", 2311, uri, sizeof(uri), meta, sizeof(meta)));
    CHECK_STR(uri, "spotify%3atrack%3a4cOdK2wGLETKBW3PvgPWqT");
    CHECK(strstr(meta, "<item id=\"00032020spotify%3atrack%3a4cOdK2wGLETKBW3PvgPWqT\"") != NULL);
    CHECK(!sonos_spotify_share("spotify:artist:0gxyHStUsqpMadRV0Di1Qt", 2311, uri, sizeof(uri), meta, sizeof(meta)));
    CHECK(!sonos_spotify_share("spotify:playlist:a\"b", 2311, uri, sizeof(uri), meta, sizeof(meta)));
    CHECK(!sonos_spotify_share("spotify:album:x", 2311, uri, 20, meta, sizeof(meta)));
}
