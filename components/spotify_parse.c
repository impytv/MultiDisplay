/* Spotify's replies and the bits of encoding the login needs (see
 * spotify_client.h); no ESP-IDF, so the host tests build it. */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "spotify_client.h"

static const char *str_at(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int int_at(const cJSON *o, const char *k, int dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}

static bool bool_at(const cJSON *o, const char *k)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

static void copy(char *dst, size_t len, const char *src)
{
    snprintf(dst, len, "%s", src);
    /* Not cut inside a UTF-8 character. */
    size_t n = strlen(dst);
    if (n == len - 1 && strlen(src) > n) {
        while (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0x80) {
            n--;
        }
        if (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0xC0) {
            n--;
        }
        dst[n] = '\0';
    }
}

static void parse_device(const cJSON *d, spotify_device_t *out)
{
    memset(out, 0, sizeof(*out));
    copy(out->id, sizeof(out->id), str_at(d, "id"));
    copy(out->name, sizeof(out->name), str_at(d, "name"));
    copy(out->type, sizeof(out->type), str_at(d, "type"));
    out->active = bool_at(d, "is_active");
    out->restricted = bool_at(d, "is_restricted");
    out->has_volume = bool_at(d, "supports_volume");
    out->volume = int_at(d, "volume_percent", -1);
}

/* The image of `images` nearest SPOTIFY_COVER_PX wide. */
static const char *pick_image(const cJSON *images)
{
    const char *best = "";
    int best_d = 1 << 30;
    const cJSON *im;
    cJSON_ArrayForEach(im, images)
    {
        const int w = int_at(im, "width", 0);
        const int d = w > SPOTIFY_COVER_PX ? w - SPOTIFY_COVER_PX : SPOTIFY_COVER_PX - w;
        if (*str_at(im, "url") && d < best_d) {
            best_d = d;
            best = str_at(im, "url");
        }
    }
    return best;
}

bool spotify_parse_player(const char *json, spotify_player_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (cJSON_IsObject(dev)) {
        parse_device(dev, &out->device);
    }
    out->active = true;
    out->playing = bool_at(root, "is_playing");
    out->shuffle = bool_at(root, "shuffle_state");
    out->progress_ms = int_at(root, "progress_ms", 0);
    copy(out->kind, sizeof(out->kind), str_at(root, "currently_playing_type"));

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
    if (cJSON_IsObject(item)) {
        copy(out->item_id, sizeof(out->item_id), str_at(item, "id"));
        copy(out->title, sizeof(out->title), str_at(item, "name"));
        out->duration_ms = int_at(item, "duration_ms", 0);
        const cJSON *album = cJSON_GetObjectItemCaseSensitive(item, "album");
        const cJSON *show = cJSON_GetObjectItemCaseSensitive(item, "show");
        if (cJSON_IsObject(album)) {
            /* A track: its artists, "A, B". */
            char *p = out->artist, *end = out->artist + sizeof(out->artist);
            const cJSON *a;
            cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(item, "artists"))
            {
                const int n = snprintf(p, (size_t)(end - p), "%s%s", p == out->artist ? "" : ", ", str_at(a, "name"));
                if (n < 0 || n >= end - p) {
                    break;
                }
                p += n;
            }
            copy(out->album, sizeof(out->album), str_at(album, "name"));
            copy(out->image_url, sizeof(out->image_url), pick_image(cJSON_GetObjectItemCaseSensitive(album, "images")));
        } else if (cJSON_IsObject(show)) {
            /* A podcast episode: the show for the artist. */
            copy(out->artist, sizeof(out->artist), str_at(show, "name"));
            copy(out->album, sizeof(out->album), str_at(show, "publisher"));
            const char *im = pick_image(cJSON_GetObjectItemCaseSensitive(item, "images"));
            copy(out->image_url, sizeof(out->image_url),
                 *im ? im : pick_image(cJSON_GetObjectItemCaseSensitive(show, "images")));
        }
    }
    cJSON_Delete(root);
    return true;
}

int spotify_parse_devices(const char *json, spotify_device_t *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, list)
    {
        if (n == max) {
            break;
        }
        parse_device(d, &out[n]);
        if (out[n].id[0]) {
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

bool spotify_parse_token(const char *json, char *access, size_t access_len, char *refresh, size_t refresh_len,
                         int *expires_in)
{
    access[0] = refresh[0] = '\0';
    cJSON *root = cJSON_Parse(json);
    const char *a = str_at(root, "access_token");
    const bool ok = *a && strlen(a) < access_len;
    if (ok) {
        snprintf(access, access_len, "%s", a);
        const char *r = str_at(root, "refresh_token");
        if (strlen(r) < refresh_len) {
            snprintf(refresh, refresh_len, "%s", r);
        }
        *expires_in = int_at(root, "expires_in", 3600);
    }
    cJSON_Delete(root);
    return ok;
}

bool spotify_url_encode(const char *in, char *out, size_t out_len)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c; c++) {
        const bool plain = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
                           *c == '-' || *c == '.' || *c == '_' || *c == '~';
        if (o + (plain ? 1 : 3) >= out_len) {
            out[0] = '\0';
            return false;
        }
        if (plain) {
            out[o++] = (char)*c;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*c >> 4];
            out[o++] = HEX[*c & 15];
        }
    }
    out[o] = '\0';
    return true;
}

void spotify_base64url(const uint8_t *in, size_t len, char *out)
{
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < len ? in[i + 2] : 0);
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        if (i + 1 < len) {
            out[o++] = B64[v >> 6 & 63];
        }
        if (i + 2 < len) {
            out[o++] = B64[v & 63];
        }
    }
    out[o] = '\0';
}
