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
 * differ. */
bool web_auth_secret_equal(const char *a, const char *b);

/* Whether a request's Host (with or without a port) names this display: its
 * IP `ip`, the setup portal's `portal_ip`, or its name `name` alone or as
 * name.local, .lan, .home, .home.arpa or .localdomain (what routers and
 * mDNS call it). Anything else is another site's domain made to point here
 * (DNS rebinding), which the cross-site check can't tell apart. */
bool web_auth_host_ok(const char *host, const char *ip, const char *portal_ip, const char *name);

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

#endif
