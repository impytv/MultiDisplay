# Reliability review (November 2026, 1.35.0)

Angle: a paranoid embedded reliability engineer looking for what makes a
wall display hang or reboot-loop after weeks of uptime: memory and handle
leaks, buffer sizes, task and lock interactions, watchdog coverage, sockets,
time handling and bad data from the network. Everything below was checked
against the code paths and their callers (and, where it matters, against
ESP-IDF 5.5 and LVGL 9.5 sources in `~/esp/esp-idf` and
`managed_components/`). Items already in `docs/improvements.md` and
`docs/lessons-learned.md` are not repeated. Nothing was built or run.

The firmware is in good shape. Most of the classic traps (cut `snprintf`s,
leaked clients, `p += snprintf` overruns, LVGL calls without the lock,
caches left pointing at memory that was freed, tick wraparound, parse
failures leaving HTTP caches "fresh") have been dealt with already. What is
left is mostly at the edges: shared resources outside the app's own code,
and draw code that trusts numbers from a server.

## Medium

### 1. The web server can take every socket the network stack has

`components/wifi_provision.c:1916-1932` (`start_web_server`), `sdkconfig:1731`

**What is wrong.** `CONFIG_LWIP_MAX_SOCKETS=10`, and the web server runs
with `HTTPD_DEFAULT_CONFIG()`'s `max_open_sockets = 7`. esp_http_server
uses 3 sockets of its own (listening socket and UDP control sockets; see
the check in `esp_http_server/src/httpd_main.c:492-505`, which only passes
because 7 + 3 = 10). With 7 browser connections open, the server holds all
10 sockets and none is left for `esp_http_client`. The server never closes
an idle connection itself: `keep_alive_enable` is off, there is no idle
timeout, and `lru_purge_enable` only closes the oldest connection when an
8th is accepted. A client that leaves without a FIN (a phone that walks out
of WiFi range or goes to sleep) keeps its socket until the nightly restart.

**Failure scenario.** Someone opens the setup page on a phone. It loads
`/setup.css`, `/setup.js`, `/dep.js`, `/status`, `/ota/status` and `/scan`,
and the browser opens up to 6 parallel connections. Then the phone leaves
the house, or the nav page stays open in another tab and polls every 5 s.
Now 8-9 of the 10 sockets are in use. The aircraft screen keeps three
clients alive at once (`adsb_client`, `adsb_routes` and `adsb_owners`), so
the routes and owner lookups fail with a connect error. With 7 sessions
open, every fetch fails (forecast, departures, the update check and an
error report's TLS connection). The screen shows "Ingen internett" until
the browser lets go, or until 02:00. Without a set clock there is no
nightly restart, so it lasts until the next power cycle.

**Suggested fix.** Set `config.max_open_sockets = 4` and
`config.keep_alive_enable = true` (with an idle time of about 30 s) in
`start_web_server`, and raise `CONFIG_LWIP_MAX_SOCKETS` to 16. That leaves
headroom for the app's 3-4 kept-alive clients, a one-off fetch and the
report task.

**Confidence:** plausible. The configuration and esp_http_server's
accounting are confirmed. That browsers really hold that many sockets on
this device has not been seen in `/log`.

### 2. A large or infinite tide level freezes the screen and loops restarts

`main/tide.c:231` (the grid loop), `components/tide_client.c:95` and `:117`
(`strtof` without a range check)

**What is wrong.** The tide parser stores whatever `strtof` returns.
Only NaN is filtered later (`isnan`), so `"inf"`, `"1e39"` or a huge
finite number all get through. The chart's scale then comes from the data:
`lo`/`hi` → `grid` → `for (float g = lo; g <= hi + 0.5f; g += grid)`, with
a `draw_line` and a `draw_text` in every pass. With `hi = inf` the
condition is always true. Once `g` passes about 2.1e9, `g + 100` rounds
back to `g` in float, so the loop never ends. A finite value of 1e7-1e8 cm
gives 10^5-10^6 passes, and each one queues an LVGL draw task.

**Failure scenario.** Kartverket's reply (or anything in its path: the
tide fetch goes through `http_get_body`, which doesn't check the
certificate, as for MET) has `<waterlevel value="inf" .../>` or
`value="99999999"` in the window shown. `tide_draw_cb` never returns, or
runs PSRAM out with draw tasks, and LVGL's assert aborts. The LVGL task
holds the adapter lock the whole time, so the weather task's
`lock_for_view` blocks too. After 60 s the software watchdog restarts the
device ("skjermen hadde stanset"), or the abort does it sooner. After the
restart the same reply is fetched again and the rotation brings the tide
screen back, so the display keeps restarting for as long as the server
sends that value. If a release on trial is running when this happens, it
also rolls back.

**Suggested fix.** In `tide_parse_series`/`tide_parse_extremes`, accept
only finite values within a sane range (say |cm| < 2000) and store NAN
otherwise. In `tide.c`, also cap the grid loop (for example at most 20
lines, or derive `grid` from `(hi - lo) / 10`).

**Confidence:** confirmed for the code path; how likely the trigger is
remains plausible (it needs a malformed or tampered reply).

## Low

### 3. Handlers are registered while the web server is already serving

`main/main.c:1246-1257`, `components/wifi_provision.c:1966-2047`,
ESP-IDF `esp_http_server/src/httpd_uri.c:126-168`

**What is wrong.** `wifi_provision_connect` starts the web server
(`start_web_server`), and only after it returns does the weather task
register about 18 more handlers. Most of those calls first unregister
`"/*"`, register the new one and then register `"/*"` again.
`httpd_register_uri_handler` and `httpd_unregister_uri_handler` take no
lock. Register stores the new `hd_calls[i]` pointer before filling in its
`uri` and `handler` (straight from `malloc`, not zeroed), and unregister
frees the entry before clearing the slot. The httpd task walks
`hd_calls[]` for every request at the same time.

**Failure scenario.** The display restarts (the nightly restart, an
update, a power cut) while a nav page tab polls `/nav.json` every 5 s, or
while a Sonos speaker still holds a subscription from before the restart
and sends a NOTIFY. A request lands in the few milliseconds of
registration. `httpd_find_uri_handler` then reads an uninitialised `uri`
pointer or a freed one, and the httpd task crashes (LoadProhibited). The
crash restarts the device, and if it happens on the first boot of a new
release, before `keep_firmware()`, the bootloader rolls the update back.
The window is small, so this would be rare.

**Suggested fix.** Register every handler before the server starts
accepting connections, either with a hook that `start_web_server` calls
right after `httpd_start` (as `wifi_provision_before_connect` does for the
clock), or by doing the registration in the httpd task through
`httpd_queue_work`.

**Confidence:** plausible. The lack of locking is confirmed in the IDF
source; the timing hasn't been observed.

### 4. An unpacked screenshot in an error report holds most of PSRAM while it is sent

`main/report.c:217-285`

**What is wrong.** `h_report` accepts up to 1.5 MB (`REPORT_MAX_IN`) and
allocates the whole body, plus the status and up to 30 KB of log, as one
PSRAM block. It holds that block until the report task's TLS POST is done.
The report task's timeout is 30 s per operation, and the browser upload may
stall up to 5 × the receive timeout. If the browser can't pack the PNG
(`small()` fails), the body is the display's own 1.1 MB screenshot.

**Failure scenario.** On a screen with ~1.3 MB of PSRAM free, an unpacked
report leaves ~150 KB free. Every forecast parse (which needs ~370 KB) and
cover decode fails in the meantime. If the site is slow (a stalled upload,
then up to 30 s to connect and 30 s to answer), the memory watchdog's 60 s
under 300 KB can trip ("minnet var nesten brukt opp") just as the user
reports a fault. When less PSRAM is free, the allocation fails instead,
and the user is told "mottak" (a receive error), which points the wrong way.

**Suggested fix.** Refuse a body that is more than, say, half of
`heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` with a 503 and a
clear message, and report the held block to the watchdog
(`wd_psram_held`) while it is sent.

**Confidence:** plausible. It needs a browser without
`CompressionStream` or a failing `small()`.

## Looked at and found sound

- **http_util**: the buffer cap and doubling, `get_stream`'s redirects and
  Content-Length reserve, the kept-alive MET client (closed on error, the
  user data reset per call), and Date-header clock setting only before the
  clock is set. Every cached client (forecast, nowcast, alerts, aurora,
  air) clears its `http_cache_t` when a parse fails, so a 304 can't keep
  stale data "fresh".
- **Rain and satellite PNG decoding**: `setjmp` frames, the `volatile` row
  pointer, size and interlace checks, and rows bounded to the crop.
- **The own PNG decoder**: chunk-length checks, palette and tRNS bounds,
  and the 4096 px limit.
- **JPEG covers**: the 4000 px header limit, and an output buffer checked
  against w×h×2 before decoding.
- **LVGL image cache**: the variable images used (RGB565 radar, satellite
  and covers, the I4 ISS map with `LV_BIN_DECODER_RAM_LOAD` off) are never
  put in the cache, so freeing their pixels in `*_release` can't leave a
  cached copy pointing at freed memory.
- **Freeing screens**: `radar_release`, `satellite_release`,
  `mgrid_release`, `iss_release` and the Spotify/Sonos cover handover all
  detach under the adapter lock and free after unlocking.
- **Locks**: LVGL → `s_mu` (Spotify), LVGL → events lock and the updater's
  `s_lock` are always taken in one order, with no reverse path.
  `report_task`'s semaphore handoff is safe, and the OTA slot claim is
  released on every `fail()` path.
- **Ticks and time**: `lv_tick_get` and TickType_t differences are
  unsigned and wrap-safe. `time_t` is 64-bit. `iso8601_to_epoch` and
  `http_date_to_epoch` are range-checked. Everything that needs the wall
  clock (nightly restart, night window, updater schedule, ISS, departures)
  checks `PLAUSIBLE_EPOCH_S` first.
- **Watchdogs**: no poll waits longer than 15 min, under the 20 min
  weather-task limit, and the updater beats every 5 % of a download.
- **Parsers outside the earlier fuzzing**: `drop_unused_numbers` in
  `yr_client.c`, `sonos_xml_unescape` (never longer than the entity it
  replaces), `sonos_logo_find`, `report_boundary`, `sgp4_parse_text`, and
  the multipart assembly in `h_report`.
- **ISS passes**: `iss_next_pass` and `follow` are bounded (4 days, 900 s
  per pass), with NaN elevations stepping forward and not looping.
