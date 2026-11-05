#include <stdarg.h>
#include <stdio.h>
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

char *buf_append(char *p, char *end, const char *fmt, ...)
{
    if (p >= end - 1) {
        return p;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(p, (size_t)(end - p), fmt, ap);
    va_end(ap);
    if (n < 0) {
        *p = '\0';
        return p;
    }
    return (n < end - p) ? p + n : end - 1;
}

bool form_tenths(const char *s, int *tenths)
{
    while (*s == ' ') s++;
    const bool neg = *s == '-';
    if (*s == '-' || *s == '+') {
        s++;
    }
    long whole = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++, digits++) {
        whole = whole * 10 + (*s - '0');
        if (whole > 10000) {
            return false;
        }
    }
    long frac = 0; /* hundredths, for the rounding */
    if (*s == ',' || *s == '.') {
        s++;
        for (int i = 0; *s >= '0' && *s <= '9'; s++, i++, digits++) {
            if (i < 2) {
                frac += (*s - '0') * (i == 0 ? 10 : 1);
            }
        }
    }
    while (*s == ' ') s++;
    if (digits == 0 || *s != '\0') {
        return false;
    }
    const long v = whole * 10 + (frac + 5) / 10;
    *tenths = (int)(neg ? -v : v);
    return true;
}

int form_hhmm(const char *s)
{
    int d[4], nd = 0, sep_at = -1;
    for (; *s != '\0'; s++) {
        if (*s >= '0' && *s <= '9' && nd < 4) {
            d[nd++] = *s - '0';
        } else if ((*s == ':' || *s == '.' || *s == ',') && sep_at < 0 && nd >= 1 && nd <= 2) {
            sep_at = nd;
        } else {
            return -1;
        }
    }
    const int hd = (sep_at >= 0) ? sep_at : nd - 2; /* hour digits; the minutes are the last two */
    if (nd - hd != 2 || hd < 1 || hd > 2) {
        return -1;
    }
    const int h = (hd == 2) ? d[0] * 10 + d[1] : d[0];
    const int m = d[hd] * 10 + d[hd + 1];
    return (h > 23 || m > 59) ? -1 : h * 60 + m;
}
