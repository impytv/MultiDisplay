#include <stdlib.h>
#include "check.h"
#include "../../components/sonos_parse.c"

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
    free(tr);
    free(pos);
    free(media);
    /* Stopped with an empty queue. */
    CHECK(sonos_parse_now("<CurrentTransportState>STOPPED</CurrentTransportState>",
                          "<TrackDuration>0:00:00</TrackDuration><TrackMetaData></TrackMetaData><TrackURI></TrackURI>",
                          "<CurrentURI>x-rincon-queue:RINCON_X#0</CurrentURI>", NULL, "1.2.3.4", &now));
    CHECK(now.empty);
    CHECK_INT(now.duration_s, -1);

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
