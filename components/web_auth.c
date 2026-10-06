#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "web_auth.h"

#define WEB_AUTH_PACE_US 1000000

bool web_auth_secret_equal(const char *a, const char *b)
{
    const size_t n = strlen(b);
    if (strlen(a) != n) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return diff == 0;
}

bool web_auth_host_ok(const char *host, const char *ip, const char *portal_ip, const char *name)
{
    if (host == NULL) {
        return false;
    }
    size_t len = strcspn(host, ":"); /* the port, if any, doesn't matter */
    if (len > 0 && host[len - 1] == '.') {
        len--; /* "multidisplay.local." */
    }
    if (len == 0) {
        return false;
    }
    const char *const exact[] = { ip, portal_ip };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        if (exact[i] != NULL && exact[i][0] != '\0' && strlen(exact[i]) == len && strncmp(host, exact[i], len) == 0) {
            return true;
        }
    }
    const size_t n = name != NULL ? strlen(name) : 0;
    if (n == 0 || len < n || strncasecmp(host, name, n) != 0) {
        return false;
    }
    static const char *const SUFFIX[] = { "", ".local", ".lan", ".home", ".home.arpa", ".localdomain" };
    for (size_t i = 0; i < sizeof(SUFFIX) / sizeof(SUFFIX[0]); i++) {
        if (len - n == strlen(SUFFIX[i]) && strncasecmp(host + n, SUFFIX[i], len - n) == 0) {
            return true;
        }
    }
    return false;
}

bool web_auth_cookie_ok(const char *cookie_header, const char *token)
{
    if (cookie_header == NULL || token == NULL || token[0] == '\0') {
        return false;
    }
    const size_t klen = strlen(WEB_AUTH_COOKIE);
    for (const char *p = cookie_header; *p != '\0';) {
        while (*p == ' ' || *p == ';') {
            p++;
        }
        const size_t pair = strcspn(p, ";");
        if (pair > klen && strncmp(p, WEB_AUTH_COOKIE, klen) == 0 && p[klen] == '=') {
            char val[80];
            const size_t vlen = pair - klen - 1;
            if (vlen >= sizeof(val)) {
                return false;
            }
            memcpy(val, p + klen + 1, vlen);
            val[vlen] = '\0';
            return web_auth_secret_equal(val, token);
        }
        p += pair;
    }
    return false;
}

const char *web_auth_next(const char *uri)
{
    const size_t n = strlen("/oppsett");
    return (uri != NULL && strncmp(uri, "/oppsett", n) == 0 && (uri[n] == '\0' || uri[n] == '?')) ? "/oppsett"
                                                                                                    : "/";
}

void web_auth_hex(const uint8_t mac[32], char out[65])
{
    for (int i = 0; i < 32; i++) {
        snprintf(out + 2 * i, 3, "%02x", mac[i]);
    }
}

bool web_auth_may_try(int64_t now_us, int64_t last_fail_us)
{
    return last_fail_us == 0 || now_us - last_fail_us >= WEB_AUTH_PACE_US;
}
