#ifndef _WEB_AUTH_H_
#define _WEB_AUTH_H_

/* The web server's checks that need no hardware (wifi_provision.c; host
 * tested): which addresses it answers to, the login cookie, and the pace
 * of password guesses. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_AUTH_COOKIE "md_auth"

/* Strings equal, compared in a time that doesn't depend on where they
 * differ, nor on `a`'s length (the one given; `b` is the secret). */
bool web_auth_secret_equal(const char *a, const char *b);

/* Whether a request's Host (with or without a port) names this display: its
 * IP `ip`, the setup portal's `portal_ip`, or its name `name` alone or as
 * name.local, .lan, .home, .home.arpa or .localdomain (what routers and
 * mDNS call it). Anything else is another site's domain made to point here
 * (DNS rebinding), which the cross-site check can't tell apart. */
bool web_auth_host_ok(const char *host, const char *ip, const char *portal_ip, const char *name);

/* How a request header read: there, not there, or there but too long for
 * the buffer (or otherwise unreadable) - which must never count as not
 * there, or a long header would get past the checks below. */
typedef enum { WEB_HDR_NONE, WEB_HDR_OK, WEB_HDR_BAD } web_hdr_t;

/* The Host check for a request: no Host at all is HTTP/1.0 without one,
 * not a browser, and let in; one that can't be read is refused. */
bool web_auth_host_hdr_ok(web_hdr_t st, const char *host, const char *ip, const char *portal_ip,
                          const char *name);

/* Whether a POST comes from a page on this display, not from another site
 * open in a browser on the home network: a browser names the page in
 * Origin (or at least Referer), and its host must be the request's Host.
 * Neither header - curl - is let in; one that can't be read, or one
 * without a matching Host, is refused. */
bool web_auth_same_origin(web_hdr_t origin_st, const char *origin, web_hdr_t referer_st, const char *referer,
                          web_hdr_t host_st, const char *host);

/* Whether a GET that changes something may: its Sec-Fetch-Site (sent by
 * every current browser) says the request came from this display's own
 * page or the address bar, not an <img> or link on another site; without
 * the header (curl, scripts) it may. */
bool web_auth_fetch_site_ok(web_hdr_t st, const char *value);

/* Whether a Cookie header holds the login cookie with value `token`. */
bool web_auth_cookie_ok(const char *cookie_header, const char *token);

/* Where to go after logging in: one of the pages, nothing else. */
const char *web_auth_next(const char *uri);

/* A 32-byte MAC as 64 hex digits and a NUL. */
void web_auth_hex(const uint8_t mac[32], char out[65]);

/* Whether a password may be tried at `now_us`: not within a second of the
 * last wrong one (`last_fail_us`, 0 = none). Wrong passwords are slowed
 * down this way rather than by sleeping, which held up the web server's one
 * task for everyone. */
bool web_auth_may_try(int64_t now_us, int64_t last_fail_us);

/* The pace above, kept per client address so that one host guessing can't
 * keep the owner out: the last few addresses with a wrong password, the
 * oldest given up when a new one comes. */
#define WEB_AUTH_PACE_SLOTS 8
typedef struct {
    uint32_t addr[WEB_AUTH_PACE_SLOTS];
    int64_t at_us[WEB_AUTH_PACE_SLOTS]; /* 0 = free */
} web_auth_pace_t;

/* Whether `addr` may try a password at `now_us`. */
bool web_auth_pace_may_try(const web_auth_pace_t *p, uint32_t addr, int64_t now_us);

/* A wrong password from `addr` at `now_us`. */
void web_auth_pace_fail(web_auth_pace_t *p, uint32_t addr, int64_t now_us);

#endif
