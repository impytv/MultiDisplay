#include "report_util.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

void report_signed_text(int64_t t, const uint8_t body_sha[32], char out[REPORT_SIGNED_MAX])
{
    int n = snprintf(out, REPORT_SIGNED_MAX, "vaertavla-feil-1\n%lld\n", (long long)t);
    for (int i = 0; i < 32; i++) {
        n += snprintf(out + n, REPORT_SIGNED_MAX - n, "%02x", body_sha[i]);
    }
}

bool report_boundary(const char *content_type, char *out, size_t out_len)
{
    static const char type[] = "multipart/form-data";
    if (strncasecmp(content_type, type, strlen(type)) != 0) {
        return false;
    }
    const char *b = content_type;
    while (*b != '\0' && strncasecmp(b, "boundary=", 9) != 0) {
        b++;
    }
    if (*b == '\0') {
        return false;
    }
    b += strlen("boundary=");
    const bool quoted = *b == '"';
    b += quoted;
    size_t n = 0;
    while (b[n] != '\0' && b[n] != (quoted ? '"' : ';') && !(!quoted && b[n] == ' ')) {
        if (!strchr("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'()+_,-./:=? ", b[n])) {
            return false;
        }
        n++;
    }
    if (n == 0 || n > 70 || n + 1 > out_len || (quoted && b[n] != '"')) {
        return false;
    }
    memcpy(out, b, n);
    out[n] = '\0';
    return true;
}
