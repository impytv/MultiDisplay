/* Helpers for the updater (main/updater.c) with no ESP-IDF dependencies,
 * so the host tests can check them. */

#include <stdio.h>
#include <string.h>

#include "version_util.h"

bool version_parse(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    return s != NULL && sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 1;
}

int version_compare(const char *a, const char *b)
{
    int va[3], vb[3];
    if (!version_parse(a, va)) {
        return -1;
    }
    if (!version_parse(b, vb)) {
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) {
            return va[i] < vb[i] ? -1 : 1;
        }
    }
    return 0;
}

void url_resolve(const char *base, const char *ref, char *out, size_t out_len)
{
    if (strstr(ref, "://") != NULL) {
        snprintf(out, out_len, "%s", ref);
        return;
    }
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    const char *end;
    if (ref[0] == '/') {
        end = strchr(host, '/'); /* the scheme and host only */
        if (end == NULL) {
            end = base + strlen(base);
        }
    } else {
        end = strrchr(host, '/'); /* base's folder */
        end = end ? end + 1 : base + strlen(base);
    }
    snprintf(out, out_len, "%.*s%s%s", (int)(end - base), base,
             (ref[0] != '/' && end[-1] != '/') ? "/" : "", ref);
}

bool hex_to_bytes32(const char *hex, uint8_t out[32])
{
    if (hex == NULL || strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        const char c = hex[i];
        const int v = (c >= '0' && c <= '9') ? c - '0'
                      : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = (uint8_t)((i % 2) ? (out[i / 2] | v) : (v << 4));
    }
    return true;
}
