#include <string.h>

#include "form_util.h"

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c |= 0x20;
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

bool form_field(const char *body, const char *key, char *dst, size_t dst_len)
{
    const size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *eq = strchr(p, '=');
        if (eq && (!amp || eq < amp) && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            const char *r = eq + 1;
            const char *end = amp ? amp : r + strlen(r);
            size_t n = 0;
            /* Decode as it goes, so the limit applies to the decoded text. */
            while (r < end && n + 1 < dst_len) {
                int hi, lo;
                if (*r == '+') {
                    dst[n++] = ' ';
                    r++;
                } else if (*r == '%' && end - r >= 3 && (hi = hex_val(r[1])) >= 0 && (lo = hex_val(r[2])) >= 0) {
                    dst[n++] = (char)((hi << 4) | lo);
                    r += 3;
                } else {
                    dst[n++] = *r++;
                }
            }
            /* Cut short: don't leave half a UTF-8 character at the end. */
            if (r < end) {
                size_t k = n;
                while (k > 0 && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) {
                    k--;
                }
                if (k > 0 && ((unsigned char)dst[k - 1] & 0x80) != 0) {
                    const unsigned char lead = (unsigned char)dst[k - 1];
                    const size_t want = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
                    if (n - (k - 1) < want) {
                        n = k - 1;
                    }
                }
            }
            dst[n] = '\0';
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    dst[0] = '\0';
    return false;
}
