#include "share_util.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

size_t share_signed_text(const char *method, const char *path, int64_t t, const char *nonce,
                         const uint8_t body_sha[32], char *out, size_t out_len)
{
    int n = snprintf(out, out_len, "vaertavla-deling-1\n%s\n%s\n%lld\n%s\n", method, path, (long long)t, nonce);
    if (n < 0 || (size_t)n + 64 >= out_len) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    for (int i = 0; i < 32; i++) {
        n += snprintf(out + n, out_len - (size_t)n, "%02x", body_sha[i]);
    }
    return (size_t)n;
}

bool share_id_ok(const char *id)
{
    if (strlen(id) != SHARE_ID_LEN) {
        return false;
    }
    for (int i = 0; i < SHARE_ID_LEN; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool group_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool share_group_ok(const char *g)
{
    const size_t len = strlen(g);
    if (len == 0 || len >= SHARE_GROUP_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!group_char(g[i])) {
            return false;
        }
    }
    return true;
}

void share_file_name(const char *id, char *out, size_t out_len)
{
    snprintf(out, out_len, "D%s.JPG", id);
}

bool share_file_id(const char *name, char *id)
{
    if (strlen(name) != 1 + SHARE_ID_LEN + 4 || (name[0] != 'D' && name[0] != 'd') ||
        strcasecmp(name + 1 + SHARE_ID_LEN, ".jpg") != 0) {
        return false;
    }
    for (int i = 0; i < SHARE_ID_LEN; i++) {
        id[i] = (char)tolower((unsigned char)name[1 + i]);
    }
    id[SHARE_ID_LEN] = '\0';
    return share_id_ok(id);
}

bool share_groups_clean(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    out[0] = '\0';
    while (*in) {
        while (*in == ',' || *in == ';' || *in == ' ' || *in == '\t') {
            in++;
        }
        char g[SHARE_GROUP_MAX];
        size_t k = 0;
        while (*in && *in != ',' && *in != ';' && *in != ' ' && *in != '\t') {
            if (k + 1 >= sizeof(g)) {
                out[0] = '\0';
                return false;
            }
            g[k++] = (char)tolower((unsigned char)*in++);
        }
        g[k] = '\0';
        if (k == 0) {
            continue;
        }
        if (!share_group_ok(g)) {
            out[0] = '\0';
            return false;
        }
        /* Not twice. */
        bool dup = false;
        for (const char *p = out; *p && !dup; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ',')) {
            dup = strncmp(p, g, k) == 0 && (p[k] == ',' || p[k] == '\0');
        }
        if (dup) {
            continue;
        }
        if (n + (n > 0) + k + 1 > out_len) {
            out[0] = '\0';
            return false;
        }
        n += (size_t)snprintf(out + n, out_len - n, "%s%s", n > 0 ? "," : "", g);
    }
    return true;
}

bool share_url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in; in++) {
        const unsigned char c = (unsigned char)*in;
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                           c == '_' || c == '.' || c == '~';
        if (n + (plain ? 1 : 3) >= out_len) {
            if (out_len > 0) {
                out[0] = '\0';
            }
            return false;
        }
        if (plain) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
    return true;
}

void share_nick_clean(const char *in, char *out, size_t out_len)
{
    const size_t cap = out_len < SHARE_NICK_MAX ? out_len : SHARE_NICK_MAX;
    size_t n = 0;
    bool space = false;
    for (const unsigned char *c = (const unsigned char *)in; *c; c++) {
        if (*c < 0x20 || *c == 0x7f || *c == ' ') {
            space = n > 0;
            continue;
        }
        /* The whole character, and the space before it, or none of it. */
        size_t len = *c >= 0xf0 ? 4 : *c >= 0xe0 ? 3 : *c >= 0xc0 ? 2 : 1;
        for (size_t k = 1; k < len; k++) {
            if ((c[k] & 0xc0) != 0x80) {
                len = 0; /* not UTF-8: left out */
                break;
            }
        }
        if (len == 0 || (*c & 0xc0) == 0x80) {
            continue;
        }
        if (n + space + len + 1 > cap) {
            break;
        }
        if (space) {
            out[n++] = ' ';
            space = false;
        }
        memcpy(out + n, c, len);
        n += len;
        c += len - 1;
    }
    if (cap > 0) {
        out[n] = '\0';
    }
}

bool share_nick_line(const char *line, char *id, char *nick)
{
    if (strlen(line) < SHARE_ID_LEN + 1 || line[SHARE_ID_LEN] != '\t') {
        return false;
    }
    memcpy(id, line, SHARE_ID_LEN);
    id[SHARE_ID_LEN] = '\0';
    if (!share_id_ok(id)) {
        return false;
    }
    char rest[SHARE_NICK_MAX + 8];
    snprintf(rest, sizeof(rest), "%.*s", (int)strcspn(line + SHARE_ID_LEN + 1, "\r\n"), line + SHARE_ID_LEN + 1);
    share_nick_clean(rest, nick, SHARE_NICK_MAX);
    return true;
}
