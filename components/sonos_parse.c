/* The Sonos speakers' replies, and what to send them (see sonos_client.h);
 * no ESP-IDF, so the host tests build it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sonos_client.h"

/* --------------------------------------------------------------------------
 * XML, as far as the replies need it
 * ------------------------------------------------------------------------ */

bool sonos_xml_escape(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    for (; *in; in++) {
        const char *e = *in == '&'    ? "&amp;"
                        : *in == '<'  ? "&lt;"
                        : *in == '>'  ? "&gt;"
                        : *in == '"'  ? "&quot;"
                        : *in == '\'' ? "&apos;"
                                      : NULL;
        const size_t k = e ? strlen(e) : 1;
        if (n + k >= out_len) {
            if (out_len) {
                out[0] = '\0';
            }
            return false;
        }
        if (e) {
            memcpy(out + n, e, k);
        } else {
            out[n] = *in;
        }
        n += k;
    }
    out[n] = '\0';
    return true;
}

/* Code point c as UTF-8 at out; its length. */
static int utf8_put(unsigned long c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

void sonos_xml_unescape(char *s)
{
    static const struct {
        const char *name;
        char c;
    } ENT[] = { { "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' }, { "&quot;", '"' }, { "&apos;", '\'' } };
    char *w = s;
    for (const char *r = s; *r;) {
        if (*r == '&') {
            bool done = false;
            for (size_t i = 0; i < sizeof(ENT) / sizeof(ENT[0]); i++) {
                const size_t k = strlen(ENT[i].name);
                if (strncmp(r, ENT[i].name, k) == 0) {
                    *w++ = ENT[i].c;
                    r += k;
                    done = true;
                    break;
                }
            }
            if (!done && r[1] == '#') {
                char *end;
                const bool hex = r[2] == 'x' || r[2] == 'X';
                const unsigned long c = strtoul(r + (hex ? 3 : 2), &end, hex ? 16 : 10);
                /* Never longer than the entity it replaces ("&#1;" is 4). */
                if (*end == ';' && c > 0 && c < 0x110000 && end > r + (hex ? 3 : 2)) {
                    w += utf8_put(c, w);
                    r = end + 1;
                    done = true;
                }
            }
            if (done) {
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* The first element `tag` from `from` on: its content's start and length.
 * NULL if there is none; a self-closed one has length 0. */
static const char *find_elem(const char *from, const char *tag, size_t *len)
{
    const size_t tl = strlen(tag);
    for (const char *p = strchr(from, '<'); p; p = strchr(p + 1, '<')) {
        if (strncmp(p + 1, tag, tl) != 0 || (p[1 + tl] != '>' && p[1 + tl] != ' ' && p[1 + tl] != '/')) {
            continue;
        }
        const char *gt = strchr(p, '>');
        if (gt == NULL) {
            return NULL;
        }
        if (gt[-1] == '/') {
            *len = 0;
            return gt + 1;
        }
        char close[48];
        snprintf(close, sizeof(close), "</%s>", tag);
        const char *end = strstr(gt + 1, close);
        if (end == NULL) {
            return NULL;
        }
        *len = (size_t)(end - gt - 1);
        return gt + 1;
    }
    return NULL;
}

/* Copy n bytes of src into out (cut to fit, not inside a UTF-8
 * character). */
static void copy_n(char *out, size_t out_len, const char *src, size_t n)
{
    if (n >= out_len) {
        n = out_len - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(out, src, n);
    out[n] = '\0';
}

bool sonos_xml_text(const char *xml, const char *tag, char *out, size_t out_len)
{
    size_t len;
    const char *s = xml ? find_elem(xml, tag, &len) : NULL;
    out[0] = '\0';
    if (s == NULL) {
        return false;
    }
    copy_n(out, out_len, s, len);
    sonos_xml_unescape(out);
    return true;
}

/* An element's text unescaped once, in a new allocation (for nested
 * XML); NULL if there is none. */
static char *xml_text_dup(const char *xml, const char *tag)
{
    size_t len;
    const char *s = xml ? find_elem(xml, tag, &len) : NULL;
    if (s == NULL) {
        return NULL;
    }
    char *out = malloc(len + 1);
    if (out != NULL) {
        memcpy(out, s, len);
        out[len] = '\0';
        sonos_xml_unescape(out);
    }
    return out;
}

/* Attribute `name` of the tag from `tag` to `end`, unescaped, into out;
 * false if it has none. */
static bool attr(const char *tag, const char *end, const char *name, char *out, size_t out_len)
{
    char key[40];
    snprintf(key, sizeof(key), " %s=\"", name);
    const size_t kl = strlen(key);
    out[0] = '\0';
    for (const char *p = tag; p + kl < end; p++) {
        if (strncmp(p, key, kl) == 0) {
            const char *v = p + kl;
            const char *q = memchr(v, '"', (size_t)(end - v));
            if (q == NULL) {
                return false;
            }
            copy_n(out, out_len, v, (size_t)(q - v));
            sonos_xml_unescape(out);
            return true;
        }
    }
    return false;
}

int sonos_parse_fault(const char *reply)
{
    char code[16];
    return reply && strstr(reply, "Fault>") && sonos_xml_text(reply, "errorCode", code, sizeof(code)) ? atoi(code)
                                                                                                       : 0;
}

/* --------------------------------------------------------------------------
 * The rooms
 * ------------------------------------------------------------------------ */

static int room_cmp(const void *a, const void *b)
{
    return strcmp(((const sonos_room_t *)a)->name, ((const sonos_room_t *)b)->name);
}

int sonos_parse_rooms(const char *reply, sonos_room_t *out, int max)
{
    char *xml = xml_text_dup(reply, "ZoneGroupState");
    if (xml == NULL || strstr(xml, "<ZoneGroups") == NULL) {
        free(xml);
        return -1;
    }
    int n = 0;
    for (const char *g = strstr(xml, "<ZoneGroup "); g && n < max; g = strstr(g + 1, "<ZoneGroup ")) {
        const char *gend = strstr(g, "</ZoneGroup>");
        const char *gtag = strchr(g, '>');
        if (gend == NULL || gtag == NULL) {
            break;
        }
        char coord[32];
        attr(g, gtag, "Coordinator", coord, sizeof(coord));
        const int first = n;
        for (const char *m = strstr(g, "<ZoneGroupMember "); m && m < gend && n < max;
             m = strstr(m + 1, "<ZoneGroupMember ")) {
            const char *mend = strchr(m, '>');
            if (mend == NULL || mend > gend) {
                break;
            }
            /* Not a pair's other half (invisible), nor a Boost. */
            sonos_room_t r = { 0 };
            char v[8], loc[96];
            if ((attr(m, mend, "Invisible", v, sizeof(v)) && v[0] == '1') ||
                (attr(m, mend, "IsZoneBridge", v, sizeof(v)) && v[0] == '1') ||
                !attr(m, mend, "ZoneName", r.name, sizeof(r.name)) || r.name[0] == '\0' ||
                !attr(m, mend, "UUID", r.uuid, sizeof(r.uuid)) ||
                !attr(m, mend, "Location", loc, sizeof(loc)) || strncmp(loc, "http://", 7) != 0) {
                continue;
            }
            /* Location="http://192.168.0.61:1400/xml/..." */
            copy_n(r.ip, sizeof(r.ip), loc + 7, strcspn(loc + 7, ":/"));
            snprintf(r.coord, sizeof(r.coord), "%s", coord);
            bool dup = false;
            for (int i = first; i < n && !dup; i++) {
                dup = strcmp(out[i].name, r.name) == 0;
            }
            if (!dup && r.ip[0]) {
                out[n++] = r;
            }
        }
    }
    free(xml);
    qsort(out, (size_t)n, sizeof(out[0]), room_cmp);
    return n;
}

/* --------------------------------------------------------------------------
 * What plays
 * ------------------------------------------------------------------------ */

/* "0:03:33" in seconds; -1 if it isn't a time. */
static int hms(const char *s)
{
    int h, m, sec;
    return sscanf(s, "%d:%d:%d", &h, &m, &sec) == 3 ? h * 3600 + m * 60 + sec : -1;
}

static bool starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* A cover address made absolute: "/getaa?..." is the speaker's. */
static void art_url(const char *art, const char *ip, char *out, size_t out_len)
{
    if (art[0] == '/') {
        snprintf(out, out_len, "http://%s:1400%s", ip, art);
    } else if (starts(art, "http://") || starts(art, "https://")) {
        snprintf(out, out_len, "%s", art);
    } else {
        out[0] = '\0';
    }
}

/* A field of a radio's "TYPE=SNG|TITLE x|ARTIST y|ALBUM z" into out. */
static void radio_field(const char *s, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    for (const char *p = s; p; p = strchr(p, '|')) {
        p += *p == '|';
        const size_t kl = strlen(key);
        if (strncmp(p, key, kl) == 0 && p[kl] == ' ') {
            copy_n(out, out_len, p + kl + 1, strcspn(p + kl + 1, "|"));
            return;
        }
    }
}

bool sonos_parse_state(const char *transport, sonos_state_t *out)
{
    char buf[32];
    if (!sonos_xml_text(transport, "CurrentTransportState", buf, sizeof(buf))) {
        return false;
    }
    *out = strcmp(buf, "PLAYING") == 0           ? SONOS_PLAYING
           : strcmp(buf, "PAUSED_PLAYBACK") == 0 ? SONOS_PAUSED
           : strcmp(buf, "TRANSITIONING") == 0   ? SONOS_TRANSITIONING
                                                 : SONOS_STOPPED;
    return true;
}

bool sonos_parse_now(const char *transport, const char *position, const char *media, const char *actions,
                     const char *ip, sonos_now_t *out)
{
    const int volume = out->volume;
    memset(out, 0, sizeof(*out));
    out->volume = volume;
    out->position_s = out->duration_s = -1;
    if (!sonos_parse_state(transport, &out->state)) {
        return false;
    }
    char buf[64];

    /* The source, from what the room plays (GetMediaInfo). */
    char uri[160] = "", track_uri[160] = "", station[SONOS_TEXT_MAX] = "", station_art[SONOS_URL_MAX] = "";
    sonos_xml_text(media, "CurrentURI", uri, sizeof(uri));
    sonos_xml_text(position, "TrackURI", track_uri, sizeof(track_uri));
    char *mmeta = xml_text_dup(media, "CurrentURIMetaData");
    sonos_xml_text(mmeta, "dc:title", station, sizeof(station));
    sonos_xml_text(mmeta, "upnp:albumArtURI", station_art, sizeof(station_art));
    free(mmeta);
    if (starts(uri, "x-sonos-vli:")) {
        snprintf(out->source, sizeof(out->source), "%s",
                 strstr(uri, "airplay")   ? "AirPlay"
                 : strstr(uri, "spotify") ? "Spotify"
                                          : "");
    } else if (starts(uri, "x-sonos-htastream:")) {
        snprintf(out->source, sizeof(out->source), "TV");
    } else if (starts(uri, "x-rincon-stream:")) {
        snprintf(out->source, sizeof(out->source), "Linje inn");
    } else if (starts(uri, "x-sonosapi-stream:") || starts(uri, "x-sonosapi-hls:") ||
               starts(uri, "x-rincon-mp3radio:") || starts(uri, "aac:") || starts(uri, "hls-radio:")) {
        out->stream = true;
        snprintf(out->source, sizeof(out->source), "Radio");
    } else if (starts(track_uri, "x-sonos-spotify:")) {
        snprintf(out->source, sizeof(out->source), "Spotify");
    }

    /* The track (GetPositionInfo). */
    char *tmeta = xml_text_dup(position, "TrackMetaData");
    /* The stream's text room for both fields of a long "TYPE=SNG|..". */
    char art[SONOS_URL_MAX] = "", stream[256] = "", cls[64] = "";
    if (tmeta && strstr(tmeta, "<DIDL-Lite")) {
        sonos_xml_text(tmeta, "dc:title", out->title, sizeof(out->title));
        sonos_xml_text(tmeta, "dc:creator", out->artist, sizeof(out->artist));
        sonos_xml_text(tmeta, "upnp:album", out->album, sizeof(out->album));
        sonos_xml_text(tmeta, "upnp:albumArtURI", art, sizeof(art));
        sonos_xml_text(tmeta, "r:streamContent", stream, sizeof(stream));
        sonos_xml_text(tmeta, "upnp:class", cls, sizeof(cls));
    }
    free(tmeta);
    if (strstr(cls, "audioBroadcast")) {
        out->stream = true;
    }
    if (out->stream) {
        /* A radio: what it plays if it says, under the station's name. */
        if (starts(stream, "TYPE=")) {
            /* Each cut to whole characters, so "artist - title" fits. */
            char t[(SONOS_TEXT_MAX - 3) / 2], a[(SONOS_TEXT_MAX - 3) / 2];
            radio_field(stream, "TITLE", t, sizeof(t));
            radio_field(stream, "ARTIST", a, sizeof(a));
            snprintf(stream, sizeof(stream), "%s%s%s", a, a[0] && t[0] ? " - " : "", t);
        }
        if (out->title[0] == '\0' || starts(out->title, "x-") || strchr(out->title, '?')) {
            snprintf(out->title, sizeof(out->title), "%s", station);
        }
        if (stream[0]) {
            snprintf(out->artist, sizeof(out->artist), "%s", out->title[0] ? out->title : station);
            copy_n(out->title, sizeof(out->title), stream, strlen(stream));
        }
    }
    art_url(art[0] ? art : station_art, ip, out->art_url, sizeof(out->art_url));
    if (sonos_xml_text(position, "RelTime", buf, sizeof(buf))) {
        out->position_s = hms(buf);
    }
    if (sonos_xml_text(position, "TrackDuration", buf, sizeof(buf))) {
        out->duration_s = hms(buf) > 0 ? hms(buf) : -1;
    }
    out->empty = out->title[0] == '\0' && out->state == SONOS_STOPPED;

    /* The buttons it takes now. */
    char act[160];
    if (actions && sonos_xml_text(actions, "Actions", act, sizeof(act))) {
        char list[168];
        snprintf(list, sizeof(list), ", %s,", act);
        out->can_play = strstr(list, " Play,") != NULL;
        out->can_pause = strstr(list, " Pause,") != NULL;
        out->can_next = strstr(list, " Next,") != NULL;
        out->can_previous = strstr(list, " Previous,") != NULL;
    } else {
        out->can_play = out->can_pause = out->can_next = out->can_previous = true;
    }
    if (out->stream) {
        out->can_next = out->can_previous = false;
    }
    return true;
}

int sonos_parse_volume(const char *reply)
{
    char v[16];
    return reply && sonos_xml_text(reply, "CurrentVolume", v, sizeof(v)) && v[0] ? atoi(v) : -1;
}

/* --------------------------------------------------------------------------
 * Favourites
 * ------------------------------------------------------------------------ */

int sonos_parse_favorites(const char *reply, const char *ip, sonos_fav_t *out, int max)
{
    char *didl = xml_text_dup(reply, "Result");
    if (didl == NULL) {
        return -1;
    }
    int n = 0;
    for (const char *it = strstr(didl, "<item "); it && n < max; it = strstr(it + 1, "<item ")) {
        const char *end = strstr(it, "</item>");
        if (end == NULL) {
            break;
        }
        /* One item at a time, so a search can't run into the next. */
        char *item = malloc((size_t)(end - it) + 8);
        if (item == NULL) {
            break;
        }
        memcpy(item, it, (size_t)(end - it));
        memcpy(item + (end - it), "</item>", 8);
        sonos_fav_t *f = &out[n];
        memset(f, 0, sizeof(*f));
        char art[SONOS_URL_MAX];
        size_t ulen = 0, mlen = 0;
        const char *u = find_elem(item, "res", &ulen);
        const char *m = find_elem(item, "r:resMD", &mlen);
        /* Whole, as escaped (unescaping only shortens them): a cut one
         * wouldn't play. */
        if (u != NULL && ulen > 0 && ulen < sizeof(f->uri) && m != NULL && mlen < sizeof(f->meta)) {
            copy_n(f->uri, sizeof(f->uri), u, ulen);
            sonos_xml_unescape(f->uri);
            sonos_xml_text(item, "dc:title", f->title, sizeof(f->title));
            copy_n(f->meta, sizeof(f->meta), m, mlen);
            sonos_xml_unescape(f->meta);
            sonos_xml_text(item, "upnp:albumArtURI", art, sizeof(art));
            art_url(art, ip, f->art_url, sizeof(f->art_url));
            n++;
        }
        free(item);
    }
    free(didl);
    return n;
}

/* --------------------------------------------------------------------------
 * Spotify, shared to Sonos
 * ------------------------------------------------------------------------ */

bool sonos_spotify_share(const char *spotify_uri, int service, char *uri, size_t uri_len, char *meta,
                         size_t meta_len)
{
    static const struct {
        const char *kind, *prefix, *key, *cls;
    } KINDS[] = {
        { "spotify:playlist:", "x-rincon-cpcontainer:1006206c", "1006206c", "object.container.playlistContainer" },
        { "spotify:album:", "x-rincon-cpcontainer:1004206c", "1004206c", "object.container.album.musicAlbum" },
        { "spotify:track:", "", "00032020", "object.item.audioItem.musicTrack" },
    };
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        const size_t kl = strlen(KINDS[i].kind);
        const char *id = spotify_uri + kl;
        if (strncmp(spotify_uri, KINDS[i].kind, kl) != 0 || id[0] == '\0' || strlen(id) > 64 ||
            strspn(id, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != strlen(id)) {
            continue;
        }
        /* "spotify:album:ID" as "spotify%3aalbum%3aID". */
        char enc[96];
        snprintf(enc, sizeof(enc), "spotify%%3a%.*s%%3a%s", (int)(kl - 9), spotify_uri + 8, id);
        const int u = snprintf(uri, uri_len, "%s%s", KINDS[i].prefix, enc);
        const int m = snprintf(meta, meta_len,
                               "<DIDL-Lite xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                               "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
                               "xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\" "
                               "xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\">"
                               "<item id=\"%s%s\" parentID=\"-1\" restricted=\"true\"><dc:title></dc:title>"
                               "<upnp:class>%s</upnp:class>"
                               "<desc id=\"cdudn\" nameSpace=\"urn:schemas-rinconnetworks-com:metadata-1-0/\">"
                               "SA_RINCON%d_X_#Svc%d-0-Token</desc></item></DIDL-Lite>",
                               KINDS[i].key, enc, KINDS[i].cls, service, service);
        return u > 0 && (size_t)u < uri_len && m > 0 && (size_t)m < meta_len;
    }
    return false;
}
