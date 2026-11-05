#include "check.h"
#include "../../components/web_auth.c"

void test_web_auth(void)
{
    /* The addresses the display answers to; another site's domain made to
     * point here (DNS rebinding) is refused. */
    const char *ip = "192.168.0.42", *portal = "192.168.4.1", *name = "multidisplay";
    CHECK(web_auth_host_ok("192.168.0.42", ip, portal, name));
    CHECK(web_auth_host_ok("192.168.0.42:80", ip, portal, name));
    CHECK(web_auth_host_ok("192.168.4.1", ip, portal, name));
    CHECK(web_auth_host_ok("multidisplay.local", ip, portal, name));
    CHECK(web_auth_host_ok("MultiDisplay.Local.", ip, portal, name));
    CHECK(web_auth_host_ok("multidisplay", ip, portal, name));
    CHECK(web_auth_host_ok("multidisplay.lan:8080", ip, portal, name));
    CHECK(web_auth_host_ok("multidisplay.home.arpa", ip, portal, name));
    CHECK(!web_auth_host_ok("multidisplay.evil.example", ip, portal, name));
    CHECK(!web_auth_host_ok("evil.example", ip, portal, name));
    CHECK(!web_auth_host_ok("192.168.0.420", ip, portal, name));
    CHECK(!web_auth_host_ok("192.168.0.4", ip, portal, name));
    CHECK(!web_auth_host_ok("multidisplay2.local", ip, portal, name));
    CHECK(!web_auth_host_ok("", ip, portal, name));
    CHECK(!web_auth_host_ok(":80", ip, portal, name));
    CHECK(!web_auth_host_ok(NULL, ip, portal, name));
    CHECK(!web_auth_host_ok("", "", portal, name)); /* no IP yet doesn't match an empty Host */

    /* The login cookie, among others or alone; a forged, cut or padded one
     * is refused. */
    const char *tok = "6dc9eca8e2f3ba337f18c03422afcc95c3434b84c2989330e0d3e3465db7f8ca";
    CHECK(web_auth_cookie_ok("md_auth=6dc9eca8e2f3ba337f18c03422afcc95c3434b84c2989330e0d3e3465db7f8ca", tok));
    CHECK(web_auth_cookie_ok("a=1; md_auth=6dc9eca8e2f3ba337f18c03422afcc95c3434b84c2989330e0d3e3465db7f8ca; b=2",
                             tok));
    CHECK(!web_auth_cookie_ok("md_auth=6dc9eca8e2f3ba337f18c03422afcc95c3434b84c2989330e0d3e3465db7f8cb", tok));
    CHECK(!web_auth_cookie_ok("md_auth=6dc9eca8e2f3ba337f18c03422afcc95c3434b84c2989330e0d3e3465db7f8c", tok));
    CHECK(!web_auth_cookie_ok("md_auth=6dc9eca8e2f3ba337f18c03422afcc95c3434b84c2989330e0d3e3465db7f8ca0", tok));
    CHECK(!web_auth_cookie_ok("xmd_auth=6dc9eca8e2f3ba337f18c03422afcc95c3434b84c2989330e0d3e3465db7f8ca", tok));
    CHECK(!web_auth_cookie_ok("md_auth=", tok));
    CHECK(!web_auth_cookie_ok("md_auth=", ""));
    CHECK(!web_auth_cookie_ok("other=1", tok));
    CHECK(!web_auth_cookie_ok(NULL, tok));
    CHECK(!web_auth_cookie_ok("md_auth=x", NULL));

    /* After logging in: back to a page, never elsewhere. */
    CHECK_STR(web_auth_next("/oppsett"), "/oppsett");
    CHECK_STR(web_auth_next("/oppsett?x=1"), "/oppsett");
    CHECK_STR(web_auth_next("/"), "/");
    CHECK_STR(web_auth_next("/oppsettx"), "/");
    CHECK_STR(web_auth_next("//evil.example"), "/");
    CHECK_STR(web_auth_next("https://evil.example/oppsett"), "/");
    CHECK_STR(web_auth_next(NULL), "/");

    uint8_t mac[32];
    for (int i = 0; i < 32; i++) {
        mac[i] = (uint8_t)(i * 37);
    }
    char hex[65];
    web_auth_hex(mac, hex);
    CHECK_INT(strlen(hex), 64);
    CHECK(strncmp(hex, "00254a6f", 8) == 0);

    CHECK(web_auth_secret_equal("abc", "abc"));
    CHECK(!web_auth_secret_equal("abc", "abd"));
    CHECK(!web_auth_secret_equal("ab", "abc"));
    CHECK(!web_auth_secret_equal("", "abc"));

    /* A password a second at most after a wrong one. */
    CHECK(web_auth_may_try(5000000, 0));
    CHECK(!web_auth_may_try(5000000, 4500000));
    CHECK(web_auth_may_try(5500000, 4500000));

    CHECK(!web_auth_secret_equal("abcd", "abc"));
    CHECK(web_auth_secret_equal("", ""));

    /* A header too long for its buffer is refused, not taken as missing. */
    CHECK(web_auth_host_hdr_ok(WEB_HDR_NONE, NULL, ip, portal, name));
    CHECK(web_auth_host_hdr_ok(WEB_HDR_OK, "multidisplay.local", ip, portal, name));
    CHECK(!web_auth_host_hdr_ok(WEB_HDR_OK, "evil.example", ip, portal, name));
    CHECK(!web_auth_host_hdr_ok(WEB_HDR_BAD, "", ip, portal, name));

    const char *h = "multidisplay.local";
    CHECK(web_auth_same_origin(WEB_HDR_OK, "http://multidisplay.local", WEB_HDR_NONE, NULL, WEB_HDR_OK, h));
    CHECK(web_auth_same_origin(WEB_HDR_OK, "http://MultiDisplay.local", WEB_HDR_NONE, NULL, WEB_HDR_OK, h));
    CHECK(web_auth_same_origin(WEB_HDR_NONE, NULL, WEB_HDR_OK, "http://multidisplay.local/oppsett", WEB_HDR_OK, h));
    CHECK(web_auth_same_origin(WEB_HDR_NONE, NULL, WEB_HDR_NONE, NULL, WEB_HDR_OK, h)); /* curl */
    CHECK(web_auth_same_origin(WEB_HDR_NONE, NULL, WEB_HDR_NONE, NULL, WEB_HDR_NONE, NULL));
    CHECK(!web_auth_same_origin(WEB_HDR_OK, "http://evil.example", WEB_HDR_NONE, NULL, WEB_HDR_OK, h));
    CHECK(!web_auth_same_origin(WEB_HDR_OK, "http://multidisplay.local.evil.example", WEB_HDR_NONE, NULL,
                                WEB_HDR_OK, h));
    CHECK(!web_auth_same_origin(WEB_HDR_OK, "http://multidisplay.local", WEB_HDR_NONE, NULL, WEB_HDR_BAD, ""));
    CHECK(!web_auth_same_origin(WEB_HDR_OK, "http://multidisplay.local", WEB_HDR_NONE, NULL, WEB_HDR_NONE, NULL));
    CHECK(!web_auth_same_origin(WEB_HDR_OK, "null", WEB_HDR_NONE, NULL, WEB_HDR_OK, h));
    /* A 200-character Origin or Referer, cut by the buffer. */
    CHECK(!web_auth_same_origin(WEB_HDR_BAD, "http://aaaa", WEB_HDR_NONE, NULL, WEB_HDR_OK, h));
    CHECK(!web_auth_same_origin(WEB_HDR_BAD, "http://aaaa", WEB_HDR_OK, "http://multidisplay.local/", WEB_HDR_OK, h));
    CHECK(!web_auth_same_origin(WEB_HDR_NONE, NULL, WEB_HDR_BAD, "http://aaaa", WEB_HDR_OK, h));

    /* A GET that changes something: not from another site's <img>. */
    CHECK(web_auth_fetch_site_ok(WEB_HDR_NONE, NULL));
    CHECK(web_auth_fetch_site_ok(WEB_HDR_OK, "same-origin"));
    CHECK(web_auth_fetch_site_ok(WEB_HDR_OK, "none"));
    CHECK(!web_auth_fetch_site_ok(WEB_HDR_OK, "cross-site"));
    CHECK(!web_auth_fetch_site_ok(WEB_HDR_OK, "same-site"));
    CHECK(!web_auth_fetch_site_ok(WEB_HDR_BAD, ""));

    /* The pace per address: one guessing doesn't hold up another. */
    web_auth_pace_t pace = { 0 };
    CHECK(web_auth_pace_may_try(&pace, 1, 5000000));
    web_auth_pace_fail(&pace, 1, 5000000);
    CHECK(!web_auth_pace_may_try(&pace, 1, 5500000));
    CHECK(web_auth_pace_may_try(&pace, 2, 5500000));
    CHECK(web_auth_pace_may_try(&pace, 1, 6000000));
    for (uint32_t a = 10; a < 10 + WEB_AUTH_PACE_SLOTS; a++) {
        web_auth_pace_fail(&pace, a, 6000000 + a);
    }
    CHECK(web_auth_pace_may_try(&pace, 1, 6000100)); /* the oldest given up */
    CHECK(!web_auth_pace_may_try(&pace, 12, 6000100));
    web_auth_pace_fail(&pace, 12, 7500000);
    CHECK(!web_auth_pace_may_try(&pace, 12, 8000000));
    int n12 = 0;
    for (int i = 0; i < WEB_AUTH_PACE_SLOTS; i++) {
        n12 += pace.addr[i] == 12 && pace.at_us[i] != 0;
    }
    CHECK_INT(n12, 1);
}
