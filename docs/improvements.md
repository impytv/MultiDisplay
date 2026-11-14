# Improvements

Findings from a review of the whole project (September 2026), most important
first within each group. Items are ticked off as they are implemented; the
planned order is 1, then 2 + 6, then 3, 7 and 10. Items 19 onwards come from
a second review after automatic updates were added; 19–21, 24–26, 29–31, 33,
39 and 41 are in firmware 1.1.0, which also moved ~16 KB of static arrays
to PSRAM (internal DRAM free went from ~53 KB to ~90 KB, lowest from 18 KB
to 52 KB).

## Stability

- [x] **1. Stuck in the setup portal after a power cut.** `sta_try_connect`
  gives up after 45 s and `portal_run()` never returns, so if the router takes
  longer than that to boot (common after an outage) the display stays in the
  setup portal until someone saves the form. A provisioned device should keep
  trying the saved network while the portal is up and restart once it
  connects. *Done: the portal retries the saved network every minute (not
  while someone is connected to the portal) and restarts once it's back.*
- [x] **2. Screen switches wait for the network.** A tap only sets
  `s_view_index`; the weather task does the actual switch (`show_view`)
  between fetches, so a tap during a fetch shows nothing for up to 15 s (30 s
  for Entur's retry). Switch the screen in the touch handler, which already
  runs with the LVGL lock, and let the task catch up. *Done: `view_enter`
  switches from the touch handler and the rotation (1–16 ms); results are
  only shown if the screen is still theirs, checked under the lock
  (`lock_for_view`).*
- [x] **3. Stale data doesn't look stale.** The weather screen's "Oppdatert
  kl." is MET's model time, not when the device last fetched it; the departure
  board shows departed buses as "Nå" (anything under a minute, including
  negative, is "Nå"), so failing fetches leave long-gone buses up; and nothing
  restarts the weather task if it hangs (no task watchdog). *Done: "Oppdatert
  kl." is the fetch time, orange "Sist oppdatert" after 30 min; departures
  more than 30 s gone are left out; a software watchdog restarts the device
  if the LVGL task stalls for 60 s or the weather task for 20 min, and the
  next boot logs which.*
- [x] **4. The nightly reboot masks leaks.** Log the minimum free internal heap
  and restart on a low threshold instead of relying only on the 02:00 reboot.
  *Done: the watchdog restarts when internal DRAM stays under 16 KB (or no
  4 KB block is left) or PSRAM under 300 KB for a minute, logs the memory
  figures - with the lowest since boot - every hour, and the next boot says
  why it restarted. The nightly restart stays.*

## User experience

- [x] **5. Wrong chart for a moment.** Switching weather location shows the
  new location's name over the previous location's chart until the nowcast
  fetch lands. *Done: without a recent drawing of that location the chart is
  hidden and "Henter værvarsel for …" shown.*
- [x] **6. Screens blank on every visit.** Radar, ships and departures clear
  their data on each visit (`s_radar_valid = false`, `s_dep_valid = false`)
  and wait for a fetch, so each rotation visit starts with "Henter…" and a new
  TLS handshake. Keep each location's last data and show it right away.
  *Done: per-location caches for aircraft (30 s), ships (5 min), departures
  (5 min) and the drawn weather screen (30 min); the rain frames are kept
  when coming back to the same location within 15 min.*
- [x] **7. Departure board gaps.** At a busy stop with "all lines", the 100
  calls fetched can all go to frequent lines, leaving rarer lines out; use
  Entur's `numberOfDeparturesPerLineAndDestinationDisplay`. Rows that don't
  fit on the screen are dropped without any sign. *Done: the query asks for
  the next two per line and destination; "+ N linjer som ikke får plass"
  under the rows ("minst" when the 24-row limit was hit).*
- [ ] **8. Navigation.** Up to 26 screens with only next/previous taps, no
  indication of position or of the rotation running. Kept as it is for now;
  ideas, roughly from least to most change:
  - A thin progress bar or "3/14" in a corner for a few seconds after each
    switch, then gone - position without permanent clutter.
  - A small marker (e.g. ⟳) while the automatic rotation is running, so a
    screen that changes by itself doesn't look like a fault.
  - Long-press anywhere to go back to the overview; a double tap to jump to
    the next location's first screen instead of the next screen.
  - Swipe up/down to move between locations and left/right between one
    location's screens (weather, radar, departures, ...), with a tap still
    stepping forward.
  - The overview as a menu: tap a location's row to go to its weather, and
    small icons in the row for its other screens.
  - A "home" screen setting: the screen to return to after the idle time
    when the rotation is off, instead of staying wherever it was left.
  *From a phone or computer this is solved since October 2026: the
  navigation page at / has a button per screen (see the README). The
  display itself still has only the taps.*
- [x] **9. Rotation at night.** Rotation (and its fetching) continues while the
  screen is dimmed; an option to pause it at night. *Done: "Pause while
  dimmed at night" on the setup page, on by default.*

## Found while testing

- [x] **14. LVGL asserts hang the device.** LVGL's default assert handler is
  `while(1);`, so a failed allocation inside LVGL froze the screen and the
  web server for good (seen once while stress-testing). *Done:
  `config/lv_assert_abort.h` aborts instead, so the device restarts with a
  backtrace; failed allocations are logged (`alloc_failed_cb`).*
- [x] **15. Internal DRAM ran out during fetches.** A forecast fetch while
  LVGL drew a screen took internal DRAM down to ~2 KB, with WiFi buffers
  failing. *Done: mbedTLS allocates from PSRAM
  (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`), and the weather task lets a newly
  shown screen draw for 500 ms before fetching; the lowest seen is now
  ~36 KB.*
- [x] **16. Drawing a screen costs ~30 KB of internal DRAM.** *Done: the
  weather screen's lv_chart bar series (rain min/max, wind/gust) added a draw
  task per bar for every 10-line strip drawn, whether the bar reached into
  it or not - about 150 small allocations at once. Each pair is now one A8
  image in PSRAM (the back bar at half opacity), one draw task per strip; a
  full redraw of the weather screen now peaks at 5.7 KB. The rain animation
  also waits until the hour's frames are in, holding the latest meanwhile.*
- [x] **17. Screenshots could exhaust PSRAM.** The 1.1 MB RGB888 snapshot plus
  a rain download or forecast parse ran PSRAM to 0. *Done: RGB565 snapshot
  (750 KB), refused with 503 when PSRAM is short.* Streaming a screenshot
  still makes WiFi drop some TX buffers.

- [x] **18. Flash writes corrupted font reads.** The app runs from PSRAM, so
  the other core keeps running during a flash write (an NVS save on every
  tap, an OTA update), and FreeType reading the memory-mapped fonts got
  garbage; LVGL then asserted (a hang before #14, a restart after). *Done:
  the font/icon partition is read with `esp_partition_read`, which waits for
  writes; LVGL's NULL asserts (one of them spurious here) are off.*

## Maintenance and security

- [x] **10. No OTA updates.** Every update needs USB. The app is 2.3 MB and the
  flash 16 MB, so two 4 MB app slots fit beside the coast and font partitions.
  *Done: `ota_0`/`ota_1` (fonts and coast kept their offsets), upload on the
  setup page or `POST /ota`, only this project's images accepted, rollback
  unless the new firmware completes a full pass of the weather task.*
- [x] **11. `main.c` is 4,500 lines.** Split per screen (weather, radar,
  rain/coast, departures). The six API clients duplicate the cJSON hooks,
  response buffer and time parsing (`yr_client.c` has its own
  days-from-civil beside `civil_time.h`, and reallocs per chunk). *Done:
  `main/` has `main.c` (start-up, screen cycle, fetch loop), `weather.c`,
  `radar.c`, `departures.c`, `draw.c`, `screenshot.c` and `watchdog.c`, with
  `app.h` for what they share; `components/http_util.c` has the response
  buffer, the one-shot GET, cJSON in PSRAM and ISO 8601 parsing for all six
  clients.*
- [x] **12. Setup page security.** No authentication, and it echoes the WiFi
  password and BarentsWatch secret into the HTML for anyone on the LAN.
  *Done: an optional password (HTTP Basic, any user name) on every page,
  the screenshot and OTA; BOOT at power-on bypasses it. Secrets are no
  longer sent in the page - a blank field keeps the saved one.*
- [x] **13. Minor.** The WiFi disconnect handler blocks the default event loop
  with `vTaskDelay`; every tap writes the current view to NVS; Entur retries a
  request that timed out (30 s total). *Done: reconnects from a timer with
  backoff (0.3 s doubling to 10 s); the last view is saved once shown for
  10 s; Entur and BarentsWatch retry only a quick failure on a reused
  connection (`http_retry_worthwhile`).*

## Second review: robustness and operations

- [x] **19. Debugging needs the USB cable, which resets the board.** Logs
  only go to the serial port, and opening it restarts the device, so the
  moment before a fault is always lost. Keep the last ~16 KB of log lines in
  a PSRAM ring buffer and serve it as `/log` (behind the setup password).
  Add a `coredump` partition (64 KB, in the 2.4 MB still free after `ota_1`
  at 0xda0000, so nothing moves) with
  `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, served as `/coredump` - crashes in
  the field become readable with `idf.py coredump-info`. *Done: `/log` (the last 16 KB of the log, with wall-clock times, in PSRAM), a `coredump` partition at 0xda0000 (binary format, CRC32) served as `/coredump` and removed with POST `/coredump/erase`; tested with a forced crash, decoded by `idf.py coredump-info`. The partition table was written once over USB.*
- [x] **20. No health view.** A `/status` JSON (and a line on the setup
  page): firmware version, uptime, reset reason, the watchdog's last trip,
  WiFi RSSI, free/lowest internal and PSRAM, the last successful fetch per
  service and the last error. Most of it exists only in the serial log. *Done: `/status` (JSON) with version, uptime, restart reason (including the watchdog's), crash dump, WiFi signal, memory and each service's last success and error; shown under Driftsstatus on the setup page with links to the log, a screenshot and the crash dump.*
- [x] **21. Ignoring MET's caching rules.** `api.met.no`'s terms ask
  clients to honour `Expires` and send `If-Modified-Since`; the forecast is
  fetched every 10 minutes per location although it changes about hourly.
  Keep `Last-Modified`/`Expires` per location and send `If-Modified-Since`:
  a 304 costs no parse, no ~350 KB of PSRAM and far less TLS time, and it
  guards against being throttled (403/429) for the whole household. *Done: forecasts, nowcasts and alerts keep MET's `Last-Modified`/`Expires` per location (`http_get_body_cached`): nothing is asked before `Expires`, then `If-Modified-Since`; a 304 keeps the data held. A tap on a weather screen still asks (conditionally).*
- [ ] **22. TLS verification is off for yr, MET alerts, adsb.lol and the
  rain radar.** It was switched off because the certificate chains didn't
  fit in internal RAM; since #15 mbedTLS allocates from PSRAM, so
  `esp_crt_bundle_attach` (already used by Entur and BarentsWatch) may now
  work for all of them. Try it and drop `CONFIG_ESP_TLS_INSECURE`.
  *Left as it is (October 2026). Turning checking off for Entur too was
  measured: departures came 0.06 s sooner (1.17 s against 1.19-1.27 s from
  the switch), within the noise, so Entur, BarentsWatch and the calendars -
  which carry a secret or private addresses - keep it; MET and Yr stay
  unchecked.*
- [x] **23. The clock depends on pool.ntp.org alone.** Add the router
  (DHCP option 42, `CONFIG_LWIP_DHCP_GET_NTP_SRV`) and a second pool as
  fallbacks. Until the clock syncs, dimming, the nightly restart and the
  update window don't run; show "Klokken er ikke stilt" if still unsynced
  after 10 minutes. *Done (1.2.0): SNTP asks the router's NTP server if DHCP names one, then pool.ntp.org and time.cloudflare.com, with no start-up delay; until NTP answers, the Date header of the first MET response sets the clock. "Klokken er ikke stilt" bottom left if it still isn't set after 10 minutes; `/status` says when and how it was set.*
- [x] **24. No visible sign of being offline.** When WiFi is down or every
  fetch fails, screens keep old data with only the weather's orange stamp.
  A small WiFi-off icon in a corner while disconnected, and each screen's
  age shown in orange past its own staleness limit (radar 2 min, departures
  3 min, ships 10 min). *Done: "Ingen WiFi" / "Ingen internett" bottom left (every fetch failing for 2 minutes); the radar, ship and departure screens show their info line in orange while the last fetch failed and older data is shown.*
- [x] **25. Only reachable by IP address.** Announce `multidisplay.local`
  with mDNS (`espressif/mdns`) so the setup page can be found without the
  router's lease table. (The display itself still uses the Pi's IP for
  updates.) *Done: `multidisplay.local` and an `_http._tcp` service (espressif/mdns, in PSRAM). LVGL is pinned to 9.5.0, as adding the component otherwise pulled in 9.6, which esp_lvgl_adapter 0.5.3 can't patch.*
- [x] **26. Cross-site requests can change the settings.** With no setup
  password, any web page opened on the home network can POST to
  `http://192.168.0.42/save` (change WiFi, locations, update address) or
  `/ota/install`. Refuse POSTs whose `Origin`/`Referer` isn't the display
  itself. Updates are safe through signing, but a changed WiFi network needs
  the BOOT button to undo. *Done: POSTs whose `Origin` (or `Referer`) names another host get 403; curl without either still works.*
- [x] **27. Settings can't be backed up.** Five locations with departure
  selections take a while to enter. `/config.json` to download (secrets
  left out) and an upload field to restore, also useful when replacing the
  board. *Done (1.2.0): **Sikkerhetskopi** on the setup page downloads `/config.json` (everything but the WiFi network and the secrets) and restores one with POST `/config.json`: checked as a whole, applied over the current settings (the display's WiFi and secrets stay; the BarentsWatch secret is dropped if the client ID changes), saved and restarted.*
- [x] **28. Host tests for the parsers.** `version_cmp`, `resolve_url`,
  `iso8601_to_epoch`, Entur's `parse_direction`/`goes_via`, the form field
  decoding and the manifest checks are pure C; build them for the `linux`
  target (or plain gcc) with a few recorded API responses, and run them in a
  GitHub Action that also does `idf.py build` (with a throwaway signing key). *Done (1.2.0): `test/host` builds the firmware's own sources (version and URL helpers, form parsing, HTTP dates and buffers, sun times, Entur selection/response/query, the ADS-B parser, route lookups, settings save/load and backup) against stub ESP-IDF headers, with AddressSanitizer and UBSan: `make -C test/host`. `.github/workflows/ci.yml` runs them and builds the firmware (signed with a throwaway key) on every push. Writing them found that the form parser cut values while still URL-encoded - a name with æøå was cut short and could end in a broken escape; fixed.*
- [x] **29. Build warnings.** `sdkconfig.defaults` still sets
  `LV_MEM_CUSTOM` and `LV_COLOR_SCREEN_TRANSP`, which LVGL 9 no longer has. *Done.*
- [x] **30. Night: dim or off.** The backlight can't dim in hardware, but
  the CH422G can switch it off. An option to turn the screen fully off
  during the night window (a tap wakes it for a minute) saves power and
  light in a bedroom. *Done: "Slå av skjermen" as an alternative to dimming; a touch lights it for a minute (the touch only wakes it).*
## Second review: features

- [x] **31. Look up places by name on the setup page.** Locations need
  typed latitude/longitude. The page already calls Entur's geocoder for
  stops; the same search (or Kartverket's place names API) can fill in name,
  latitude and longitude. *Done: "Finn sted" in each location block searches Kartverket's place names from the browser and fills in name, latitude and longitude.*
- [x] **32. Weather: the week ahead.** The forecast already holds ~9 days;
  a row of day cards (symbol, max/min, rain sum, wind) under or instead of
  the 48-hour chart, or as its own screen. *Done (1.4.0): the forecast is summed up per local day (high/low, precipitation, strongest wind, the 6-hour symbol nearest midday); a "Uke" screen shows seven day cards with the temperature range as bars on a scale shared by the week.*
- [x] **33. Weather: sunrise and sunset.** MET's `sunrise/3.0` gives sun
  and moon times; show them on the weather screen and shade the chart's
  night hours. The dark theme could also follow sunset automatically. *Done: "Sol 07:15–18:58" (or Midnattssol / Mørketid) in the weather header when there is room, and the night hours shaded in both charts, from the sun's elevation worked out on the device (`main/sun.c`, checked against published times for Oslo and Tromsø). The theme doesn't follow sunset.*
- [x] **34. Rain is coming.** The nowcast already fetched says when
  precipitation starts or stops in the next 90 minutes; a line such as
  "Regn om 25 min" on the weather screen and in the overview. *Done (1.4.0): "Nedbør om 25 min", "Opphold om 10 min" or "Nedbør den neste timen" in blue in the weather header, in place of the sun times, from the nowcast already fetched (only the location on screen has one, so not in the overview).*
- [ ] **35. Departures: walking time.** Per stop, hide departures sooner than
  the time it takes to walk there, and show "Gå nå" when it is time to leave
  for the next one.
- [x] **36. Departures: disruptions.** Cancellations are parsed; show them
  struck through, and Entur's `situations` (track work, replacement buses)
  as a line under the affected row. *Done (1.4.0): Entur's notices (situations) for a row's departures as an orange line under it, e.g. "Færre vogner: Denne avgangen kjører med 4 vogner..."; cancellations already said "Innstilt".*
- [ ] **37. Electricity prices.** Today's and tomorrow's hourly spot price
  for the chosen price area (NO1–NO5) from hvakosterstrommen.no, as a bar
  chart with the current hour marked and the cheapest hours highlighted.
- [x] **38. More screens from open Norwegian data.** *Air quality and pollen
  done (1.3.0): a "Luft" screen per location with MET's air quality
  forecast and CAMS pollen via Open-Meteo (NAAF's own API needs an
  agreement). Tides and a calendar done (October 2026): a "Tidevann" screen
  per location (Kartverket's forecast, tide and measured level for the
  nearest station, the next high and low tides; "too far from the coast"
  inland), and one "Kalender" screen for the display after the overview,
  merging up to three secret iCal addresses (two weeks as an agenda;
  streamed, so any size; recurring events expanded; addresses kept out of
  backups). Tide took the last free bit of the per-location screen flags
  (`show[]` is a uint8_t): another per-location screen needs them widened,
  with a settings migration.*
  - Tides and water level (Kartverket's API) for coastal locations.
  - Air quality (MET's airqualityforecast).
  - Pollen in season.
  - A calendar from an iCal URL (Google/Outlook share links).
- [x] **39. Aircraft: routes.** adsb.lol's `/api/0/routeset` gives origin and
  destination by callsign: "SAS123 OSL→BGO" in the table. *Done: routes from adsb.lol's per-callsign route files, kept a day (96 in PSRAM), up to four new lookups per poll; shown as "OSL-BGO" in the Rute column, or only the destination ("Til") when a large font leaves too little room.*
- [ ] **40. Home Assistant.** Publish the display's state over MQTT (and
  accept a "show screen X" command), or show a few Home Assistant sensors
  (indoor temperature, door) on the overview.
- [x] **41. One language.** The screens are Norwegian and the setup page is
  English; pick one for both (or a language setting). *Done: the setup page, its messages, the update statuses and the update site's index page are Norwegian. The serial log stays English.*
- [ ] **48. Ships from our own receiver.** BarentsWatch leaves out small
  vessels (fishing boats under 15 m, leisure boats under 45 m). An RTL-SDR
  dongle and a VHF antenna on the Pi, decoded by
  [AIS-catcher](https://jvde-github.github.io/AIS-catcher-docs/) (GPLv3,
  Docker image `ghcr.io/jvde-github/ais-catcher`), would give every ship in
  range, typically 20–40 km depending on antenna height, with no API key.
  AIS-catcher can also read network feeds into the same ship table (raw
  NMEA over TCP, for example Kystverket's open AIS stream, or `wss://`),
  so places out of the antenna's reach can be covered too. Its web viewer
  (`-N 8100`) serves every ship at `/api/ships_array.json` along with a
  live map. A small proxy on the Pi would keep only the ships in each
  display's circle and answer in BarentsWatch's field names, so
  `ais_client.c` needs only a plain HTTP GET (no TLS, no token) and a
  ship source setting. aisstream.io was considered as well: it allows only
  3 connections per IP and sends nothing on connect, so it too would need
  the proxy, and AIS-catcher can't read its JSON. Scraping MarineTraffic
  breaks its terms.


## Third review: memory (October 2026)

Static internal DRAM was already clean (no app array over 2 KB left in it);
these are the runtime allocations and the code placed in internal RAM. Before:
65 KB internal free and 30 KB lowest after 8 minutes on 1.4.2. After 42-44:
161 KB free and 114 KB lowest after 10 minutes of the normal rotation;
with all seven screen types on all five locations and one full rotation
(36 screens, 13 minutes), 159-164 KB free, 116 KB lowest, largest block
117 KB, PSRAM lowest 690 KB.

- [x] **42. LVGL's small allocations in internal DRAM.** LVGL used the C
  library's `malloc`, so everything under the 1 KB PSRAM threshold - every
  widget, style, label text and FreeType glyph-cache entry (FreeType
  allocates through LVGL) - came from internal DRAM. *Done: LVGL's own
  allocator (`CONFIG_LV_USE_CUSTOM_MALLOC`, `main/lv_mem_psram.c`) prefers
  PSRAM and falls back to internal DRAM.*
- [x] **43. Task stacks in PSRAM.** *Partly done: only the setup portal's
  DNS task (3 KB) moved. The weather task and the web server keep internal
  stacks: flash reads and writes are fine from a PSRAM stack here, but the
  OTA functions memory-map flash, which freezes the cache and asserts on a
  PSRAM stack - moving the weather task crash-looped the device (see
  docs/lessons-learned.md). The reboot task is short-lived and left alone.*
- [x] **44. WiFi code in internal RAM.** `ESP_WIFI_IRAM_OPT` and
  `ESP_WIFI_RX_IRAM_OPT` put ~19 KB of WiFi library code in internal RAM,
  shared with the heap on the S3, for throughput the display doesn't need.
  *Done in 1.4.3 (both off; `idf.py size` DIRAM 150.7 KB → 132.7 KB), undone
  in 1.5.1: with WiFi's code running from PSRAM, the RGB panel's feed from
  PSRAM was refilled late and the picture jumped on the rain radar. 1.5.1
  also doubles the panel's bounce buffers (20 lines) and resynchronises it
  every frame (`CONFIG_LCD_RGB_RESTART_IN_VSYNC`). That setting turned out to
  cause the bottom lines showing at the top: ESP-IDF 5.5 then never resets
  its refill count, so one missed refill interrupt shifted the picture by a
  bounce buffer until the next glitch or restart. 1.5.4 turns it off again,
  creates the panel on core 1 (away from WiFi) with an IRAM-safe
  interrupt, and keeps the 16 MHz pixel clock.*
- [ ] **45. Unused JPEG decoder in IRAM.** `esp_lv_decoder` always links
  `esp_new_jpeg`, whose assembly (6.3 KB) is placed in internal RAM; only
  PNG is used. Removing it needs a local copy of the decoder component.
- [ ] **46. LVGL's partial draw buffer.** `esp_lvgl_adapter` always puts the
  800×10 draw buffer (16 KB) in internal DRAM, whatever `use_psram` says
  (`display_manager.c`). Moving it needs a patched adapter, and drawing
  would be slower.
- [x] **47. Stack use in `/status`.** Nothing shows how much of each task's
  stack is used, so stack sizes are guesses. Add each task's high-water mark
  (needs `CONFIG_FREERTOS_USE_TRACE_FACILITY`) and size the stacks from it.
  *Done: `/status` lists every task's lowest free stack in bytes
  (`stabler`, tightest first, with whether it is in PSRAM); +2.3 KB internal
  RAM for the trace functions. Least free, after 10 minutes of the normal
  rotation and after a full rotation with every screen type on all five
  locations: httpd 1056 B (of 6 KB) at worst, tiT 1080 B (3 KB), yr_weather
  4140 B (8 KB), lvgl 6712 B (12 KB, PSRAM), swdraw 13856 B (32 KB, PSRAM),
  esp_timer 3048 B (3.5 KB). No stack is resized yet: yr_weather and
  esp_timer have room, but stacks have overflowed here before.*

## Fourth review: robustness and speed (October 2026, 1.8.1)

- [x] **49. A tap redraws the weather screen two or three times.** A tap
  wakes the weather task while it waits out `VIEW_SETTLE_MS`; that wake is
  still pending after the first poll, so `ulTaskNotifyTake` returns at once
  and the nowcast is fetched and the chart redrawn again (seen: Vestpollen
  merged at 245.1 s and 245.8 s). Then the 10 s wake to save the last view
  polls a third time (Kvaløysletta 250.1 s and 260.3 s). Each is a TLS
  request and a full chart redraw. Clear the pending wake when a switch is
  adopted, and let the save wake save without polling. *Done: the switch's
  own wake is cleared once it is adopted, and the wait saves the view
  without ending.*
- [x] **50. A failed aurora fetch waits an hour.** One transient failure at
  start-up leaves the weather screens without aurora marking for an hour.
  Retry a failure after 10 minutes; keep the hour for successes. *Done in
  1.32.0 (`AURORA_FAIL_RETRY_MS`).*
- [ ] **51. A satellite fetch that breaks off loses the image shown.** Rows
  are decoded straight into the views, so a reply cut short leaves them part
  old, part new and the screen goes back to "Henter...". Keeping the old
  image needs a second buffer (360 KB for Europe), so it is left as is
  unless it is seen to happen.
- [ ] **52. Screenshots hold the display for up to 20 s.** Taps and the
  rotation wait while a slow client downloads. Acceptable for a debugging
  tool; a shorter cap (e.g. 8 s) would bound it.
- [ ] **53. The setup password is stored in plain text.** It is kept as
  typed in NVS, readable by anyone with the board and a USB cable. A salted
  hash (PBKDF2 or similar) would do, as the display only compares it; the
  login cookie is already derived from it, so it needs a one-time migration
  of the saved setting.

## Fifth review: the whole codebase (October 2026, 1.9.0)

A clean configuration from `sdkconfig.defaults` (as CI builds) was checked
against the local `sdkconfig`: identical, so nothing the build relies on
lives only in the untracked file.

- [x] **54. DNS rebinding gets past the cross-site check.** `same_origin`
  compares Origin with the request's own Host. A web page whose domain is
  made to resolve to the display's address (DNS rebinding) sends Origin and
  Host both as its own domain, so the check passes; with no setup password
  that page can then POST `/save` (WiFi, password, update address),
  `/config.json` or `/ota/install`. Accept only requests whose Host is the
  display's IP, `<name>.local` or the portal's 192.168.4.1 (with any port),
  and refuse the rest with 403. Updates stay safe through signing either
  way, but a changed WiFi network or password needs the BOOT button.
  *Done: every request but the stylesheet must name the display (its IP,
  192.168.4.1, or its name alone or with .local, .lan, .home, .home.arpa
  or .localdomain, any port); others get 403. Not in the setup portal,
  whose captive-portal probes name other sites.*
- [x] **55. Wrong BarentsWatch credentials are retried every 30 s.** A token
  request rejected with 400/401 is asked again on every ship poll while the
  ship screen is on show, which risks the client being blocked. Wait 10
  minutes after a rejection (a new setting restarts the display anyway).
  *Done: after a rejection the token isn't asked for again for 10 minutes;
  the screen keeps saying "Innlogging feilet".*
- [x] **56. A wrong password stalls the web server for a second.** The
  delay after a wrong password is a `vTaskDelay` in the single httpd task,
  so anyone on the network can keep the pages and the screenshot slow by
  sending wrong passwords. Remember the time of the last failure and answer
  429 to attempts within a second instead of sleeping.
  *Done: a password is checked at most once a second after a wrong one;
  one sooner gets 429 (or "Vent et sekund" on the login form) at once, and
  no request waits.*
- [x] **57. The overview's columns follow the forecast, not the clock.**
  `update_overview` takes "now" from the newest forecast's first point (a
  leftover from before the clock was set over NTP). After hours offline the
  columns start hours back. Use the clock when it is set, and the forecast
  points from the current hour on.
  *Done: the columns start at the clock's hour once it is set; a column
  the forecast held doesn't reach within 3 hours shows a dash.*
- [x] **58. A lost "Check now" or "Install now".** `updater_poll` reads and
  clears `s_request` without `s_lock`, while the web server sets it under
  the lock; a request landing between the two is lost and the page shows
  "busy" until the next scheduled check. Take the lock for the swap.
  *Done.*
- [x] **59. The login code has no host tests.** The cookie token, the
  cookie's parsing and the `next` address check live in `wifi_provision.c`,
  which the host tests don't build. Move them into a small file of their
  own (like `form_util.c`) and test them, including a forged and a
  truncated cookie.
  *Done: `components/web_auth.c` (the address check, the cookie, the
  address after logging in, the pace of tries), with 44 host checks. The
  HMAC itself is mbedTLS's.*
- [x] **60. Night dimming and the nightly restart can start late.** They
  are checked when the weather task wakes; on the satellite screen with the
  rotation paused for the night that can be 15 minutes apart. Check them
  from the once-a-second LVGL timer instead (the restart from a task).
  *Done: both run from the once-a-second LVGL timer.*

## Sixth review: a fresh look at failure and speed (October 2026, 1.9.1)

- [x] **61. An unknown flight's route is looked up every 5 s.** For a
  callsign adsb.lol has no route for, it answers 404 with a 10 KB GitHub
  Pages error page. That is over the 8 KB cap in `adsb_routes.c`, so the
  answer counts as a failed request, not as "no route": it isn't cached,
  the connection is closed, and the lookups for the aircraft after it in
  the list are skipped. On the aircraft screen, every 5 s poll then opens
  a new TLS connection for the same callsign and still shows no routes
  (seen on 1.9.1: six "Response over 8192 bytes" in one poll). Keep only
  the body of a 200 reply, in `adsb_routes.c` and in `http_util.c`'s
  `get_body_handler`, where an error page over the cap is logged as "over
  N bytes" instead of its HTTP status.
  *Done: both keep a body only from a 200 reply; the routes screen no
  longer logs "Response over" and the routes service reports OK.*
- [x] **62. Weather icons are decoded again on every redraw.** LVGL's image
  cache is off (`CONFIG_LV_CACHE_DEF_SIZE=0`, LVGL's default, never set
  here), and so is its header cache. Every time an icon is drawn,
  `esp_lv_decoder` reads its PNG from the font partition and decodes it.
  With the 10-line draw buffer, a 48 px icon is drawn in five or six
  strips, each decoding it again. A full redraw of a weather screen
  decodes around 80 icons and 75 wind arrows; the overview's 20 icons are
  decoded about 80 times. Give the cache about 256 KB (it is allocated
  from PSRAM since #42, as an icon is 9 KB decoded) and the header cache
  32 entries, then compare the redraw time on the device.
  *Done: 256 KB cache and 32 headers. Full redraws measured on the
  device (`lv_refr_now` after invalidating the screen, average of five):
  weather 516 → 180 ms, overview 547 → 160 ms; screens without icons
  unchanged (aircraft 285 ms, departures 184 ms). PSRAM lowest 1.07 MB
  after the overview and every weather screen.*
- [x] **63. The firmware is built for debugging (`-Og`).** No optimization
  level is set in `sdkconfig.defaults`, so ESP-IDF's default `-Og` is
  used, for LVGL's software rendering, PNG and JSON decoding and the radar
  too. Built with `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`): 33 KB more
  code in flash (the slot is 4 MB, the image 2.6 MB) and 5 KB *less*
  internal RAM used (DIRAM 154.7 → 149.4 KB). Not measured on the device
  yet. Check the redraw times and the stack high-water marks in
  `/status` before adopting it, as `-O2` may use more stack.
  *Done: adopted. On top of #62: overview 160 → 142 ms, weather 180 →
  168 ms, aircraft 285 → 266 ms, departures 184 → 169 ms, the forecast's
  JSON 148 → 130 ms. Lowest free stack after a pass of the screens
  about as on `-Og`: yr_weather 4336 B (was 4140), lvgl 6288 B (6712),
  httpd 3024 B.*
- [x] **64. Every MET request makes a new TLS connection.** Forecasts,
  alerts, nowcasts, aurora, air quality, tides and satellite images each
  open a new HTTPS connection (`http_get_body_cached`), with a full
  handshake each time. Only the rain radar keeps its connection. A
  weather screen makes two or three requests to api.met.no per poll, and
  the overview up to ten. One kept-alive client for api.met.no, closed on
  screens that don't use it (as the others are), would save most of the
  handshakes.
  *Done: `http_util` keeps one connection to api.met.no (forecasts,
  alerts, nowcasts, air quality), retried once on a fresh one if the
  server closed it, and closed on screens that don't ask MET. On the
  overview each location's forecast and alerts now take 0.5 s together,
  while one aurora request on a new connection takes 0.53 s.*
- [x] **65. An upload and an automatic update can write the same slot.**
  `POST /ota` (httpd task) and the updater's download (weather task) both
  write to `esp_ota_get_next_update_partition()`, and `esp_ota_begin` does
  not refuse a second writer. An upload during the scheduled install
  interleaves both images. The signature check at the end catches a mixed
  image, but one writer may already have selected the slot and be
  restarting while the other erases it. Take one lock (or the updater's
  `s_busy`) around both.
  *Done: `ota_writer` lets one writer at a time claim the slot ("En
  annen oppdatering pågår allerede" for a second), kept after a
  successful finish until the restart. Two uploads from the web can't
  overlap anyway (one httpd task); only the updater against an upload
  could, which can't be shown without a newer release to install.*
- [x] **66. New firmware is kept even if nothing can be fetched.**
  `keep_firmware()` marks a new image valid after the first pass of the
  weather loop, whether or not any fetch in it worked. A release that
  draws but can't fetch (TLS settings, a broken client) stays installed.
  Mark it valid on the first successful fetch (diag's first success)
  instead, without rolling back on failures alone.
  *Done: kept after the first pass in which any fetch has worked
  (`diag_any_ok`). Seen: "New firmware works" 5 s after boot, with the
  departures fetched.*
- [x] **67. Polling goes on with the screen switched off.** With "screen
  off at night", the screen on show is still polled all night: the
  aircraft screen asks adsb.lol every 5 s, and departures every 30 s.
  While the backlight is off, poll at most every 5 minutes, and right
  away when a touch lights it.
  *Done: while the backlight is off the wait is at least 5 minutes, the
  automatic rotation stands still (it switched screens every 30 s, each
  fetching, with "Stopp om natta" off), and lighting the screen, by a
  touch, the web or the end of the night, polls at once. Seen: departures
  fetched at 21:06 and 21:11 with the screen off, and at once when lit.*

## Seventh review: the whole codebase again (October 2026, 1.9.2)

What the fifth and sixth reviews looked at was not gone over again; this
pass read the calendar, satellite, departures, rain, ship and clock code,
the setup page's builders and handlers, the update server's nginx setup
and CI (green for 1.9.1 and 1.9.2).

- [x] **68. The setup page could write past its buffer.** `build_page`
  appends with `p += snprintf(p, end - p, ...)` about 50 times. Once one
  call is cut short, `p` points past `end`, and the next call's size,
  `end - p`, is negative and turns into a huge `size_t`: it writes beyond
  the 36 KB buffer instead of stopping. The page is 28.9 KB with five
  locations and full departure settings, so this needs a few more
  sections to happen, and would then corrupt the heap instead of just
  cutting the page short. Append through a helper that stops at `end`.
  *Done: every append in the setup page goes through `buf_append`
  (`form_util.c`, host-tested), which stops at the end; the page is
  byte-identical to before. Found on the way: the WiFi scan list was
  HTML-escaped, but the page uses the names as plain values, so a network
  called "A&B" was offered as "A&amp;B" (and saved wrong if picked), and
  one with a backslash broke the list. It is built with cJSON now.*
- [x] **69. A calendar that fails once disappears for 15 minutes.** With
  more than one calendar, a fetch where one of them fails (a timeout at
  Google, say) still counts as done: the list is replaced without that
  calendar's events, "(kalender N mangler)" is shown, and nothing is
  asked again for 15 minutes. Keep the failed calendar's events from the
  last list, and retry within a minute. The note also names only the last
  missing calendar when two fail.
  *Done: a calendar that fails keeps its events from the last list (for
  as long as a list is shown at all), is retried once a minute later,
  and the note names every one: "(kalender 1 og 3 ikke oppdatert)".
  Tested with a second calendar served from the Pi and then removed:
  its event stayed with the note, and the retry 62 s later read it.*
- [x] **70. A too-large settings form is saved cut short.** `h_save` reads
  at most 12 KB and saves whatever arrived. A checkbox missing from the
  cut-off part counts as unticked, so a too-long form would silently
  switch settings off. The form is 1.5 KB today, so this is out of reach,
  but `PUT /config.json` already refuses a too-large body; do the same.
  *Done: a form over 12 KB is refused with 413 and nothing is saved
  (`PUT /config.json` answers 400 to the same). Tested with a 13 KB
  form: 413, settings unchanged.*

## Eighth review: fuzzing what comes from the network (October 2026, 1.9.4)

Three reviews have read the code; this one ran it on bad input. GCC's
`-fanalyzer` found nothing in the parsers and config code. A mutation
fuzzer (a few hundred lines, not in the repository) fed each parser that
reads data from the network a real reply, cut, spliced and corrupted, with
AddressSanitizer and UBSan: the calendar 600,000 times, the ADS-B, tide,
aurora, air, pollen, alert and nowcast replies 20,000-200,000 times each,
the forecast 3,000 times.

- [x] **71. Calendar durations and intervals could overflow.** `ical.c`
  added up a `DURATION` in a `long` with no limit, and stepped a
  recurrence by `k * INTERVAL` in a `long`. That is 64 bits on the host
  but 32 on the ESP32, so `DURATION:P30000D` or a large `INTERVAL` would
  overflow on the display (undefined behaviour; in practice wrong times,
  or events vanishing). Found within 3 seconds of fuzzing. *Done: the
  duration's digits stop before they can overflow, and the recurrence
  arithmetic is `int64_t`.*
- [x] **72. Dates from servers weren't range-checked.**
  `iso8601_to_epoch` and `http_date_to_epoch` read numbers of any length
  and offsets of any size, so a garbled date overflowed `int` before
  reaching `days_from_civil`. *Done: field widths in the `sscanf`
  formats, years 1900-2200, months, days, hours, minutes, seconds and
  zone offsets checked; anything else reads as "no date" (0), as a
  malformed one already did. Four host tests added.*

Nothing else turned up: no out-of-bounds read or write, and no other
undefined behaviour in the hand-written ADS-B and tide parsers or the
cJSON-based ones.

## Found in use (October 2026, 1.9.4)

- [x] **73. The ship radar sometimes had no water.** Reported with a
  screenshot: the coastline drawn, the sea left white. The water fill
  allocated a 500 KB queue (a pixel index for every pixel of the disc)
  each time a coastline was drawn, and drew no water when that failed -
  without trying again. With the rain radar's frames (up to 1 MB, kept
  after the screen is left) and the image cache (#62, 256 KB) in PSRAM,
  a block that size was often not there: the lowest free PSRAM was 374 KB
  after a morning's rotation. *Done: the fill floods each area twice
  (count the votes, then fill) through a 64 KB ring of the area's edge,
  static in PSRAM, so it allocates nothing. The water came out
  pixel-identical to before on the ship screen, and was drawn on the 150 km
  aircraft and 100 km rain screens with no queue overflow.*
- [x] **74. The rain radar could run PSRAM out and restart the display.**
  With the screens set up since (satellite close-ups for two locations,
  the Europe image, departures and tides), 963 KB of PSRAM was free after
  start-up. Opening the rain radar at 100 km then sized its hour of
  frames to its full 1 MB budget: PSRAM went down to 19 KB, the fetch
  failed ("Kunne ikke hente nedbør"), and the watchdog restarted the
  display ("minnet var nesten brukt opp"). *Done: the frames are sized
  to what PSRAM has beyond a 512 KB reserve (coarser cells only if
  needed), and the water fill's seed points take 6 bytes instead of 16
  (512 → 192 KB), so 1,261 KB is free after start-up. A pass through all
  20 screens: rain at full resolution, PSRAM lowest 614 KB, no restart.
  `/status` now also gives PSRAM's largest free block, and the rain
  client's log gave every image as "0 bytes" (logged after freeing it).*
- [x] **75. The month calendar's holidays end with 2029.** They are built in
  from norskkalender.no (`scripts/build_holidays.py`), which publishes them
  up to 2029; from 2030 only Sundays are red. Run the script again when the
  site has later years (it reads on until a year has none) and rebuild. The
  setup page says which year they go to. Computing them (Easter and the
  days that follow it) would remove the limit. *Done in 1.32.0: computed
  (`components/holidays.c`, Easter by Meeus's algorithm), checked against
  the table for 2026-2029 before it and `scripts/build_holidays.py` were
  removed. They agreed but for 17 May 2027, which the site called only
  "2. pinsedag"; two on one day are now both named.*
- [ ] **76. The month calendar's astronomy ends with 2035.** The phases,
  seasons, meteor showers and eclipses are built in for 2026-2035 by
  `scripts/build_astro.py` (Skyfield and DE440s). Run it again with later
  years before then and rebuild.
- [ ] **77. Police, rescue and ambulance aircraft are found by name.** The
  aircraft screen reads the owner and operator from Luftfartstilsynet's web
  page for each Norwegian registration (no API; ~46 KB a page, scanned for
  `operator` and `mainOwner` as it arrives), and knows the rescue and
  ambulance services' callsign prefixes (SAVER, DOC, BNO, DFL). If the page
  changes, the lookups fail ("Luftfartøyregisteret" in `/status`) and only
  the callsigns work; a new operator or callsign scheme needs adding in
  `components/adsb_owners.c`. Ambulance planes owned by leasing companies
  with no operator registered are found only by callsign.
- [x] **78. Some sources were asked again every 30-60 s while failing, and
  everything every 5 min at night.** The satellite image (large) was asked
  for every minute after a failure even with the last image on show; air
  quality every 30 s while nothing was shown - for good, at a place MET
  doesn't cover - and pollen every poll. With the screen switched off for
  the night, the screen on show still fetched every 5 minutes. *Done:
  after a failure these wait 30 s / 1 min once, then 5 minutes
  (`backoff_t` in `main/app.h`); with the screen off nothing is fetched
  (the kept-alive connections are closed) until it is lit, except NTP, the
  update check, and a firmware on trial proving itself. A 6-minute serial
  log with the screen off showed no fetches. Still quick on failure, but
  cheap or bounded: forecast and alerts (every poll, 5 min), departures
  (15 s), tide and rain (30 s while nothing is shown), missing past rain
  frames (each poll for up to an hour).*
- [x] **79. A location's departures fitted in 199 characters.** Five lines
  with Flybussen's 45-character ID filled it, and the setup picker refused
  more. *Done (with the departure groups, 1.19.0): 399 characters; the
  stored "deps" blob of 200-byte texts is converted when loaded (host test
  in `test_config.c`, and a device kept its boards across the update).*

## Ninth review: what changed since 1.9.4 (October 2026, 1.22.0)

The code added or changed since the eighth review (the month calendar
and its astronomy, holidays and calendar events, the clock screen and the
chip's temperature, departure groups, police/rescue/ambulance aircraft,
alert bars and aurora bars on the weather screen, the setup page's new
fields), read in full, and the rest searched for unsafe string handling,
unchecked lengths from the network and HTML built from data. The setup
server's login, Host and Origin checks, the updater's manifest and image
checks, and the escaping on the setup and navigation pages hold.

- [x] **80. The list of screens can overflow.** `s_stops` in `main/main.c`
  has room for 4 + 9 per location = 49 screens, but the clock (1.18.0)
  made it five shared screens: overview, calendar, month, clock and
  Europe. With all five ticked and all nine screens at five locations,
  `build_stops` writes a 50th entry past the array, into whatever static
  follows it. Size it 5 + 9 per location, ideally from the counts
  themselves so the next screen can't repeat it. *Done (1.22.1):
  `STOPS_GLOBAL` + `STOPS_PER_LOC` per location, with a static assert that
  the per-location list matches, and an assert on the count.*
- [x] **81. Busy calendars lose the month calendar's later weeks
  silently.** The month fetches its 35 days into the same list as the
  calendar screen's 14, which keeps the earliest 96 events
  (`CAL_EVENTS_MAX`). A daily event in each of three calendars is 105 in
  five weeks, so the last days come out empty, with no sign that anything
  is missing. A list of its own with room for more (256 events is 26 KB
  of PSRAM, twice), or a note when it is full. With a bigger list the
  month could also fetch a wider window once, so the arrow buttons don't
  download every calendar again at each step. *Done (1.22.1): a list now
  has a capacity of its own (`cal_list_new`); the calendar screen keeps 96,
  the month 256 (host test). The wider window followed in 1.23.0: four
  weeks before and eight after those shown, 640 events, and the days after
  a full list's last one left without.*
- [x] **82. The month calendar's events go missing at the start of a
  week.** Its window starts on the Monday of this week; at Monday
  midnight the grid moves on, but the events held are for the old window
  and aren't shown until the next poll, up to 5 minutes later. Wake the poll
  (`app_wake_poll`) when the first day shown changes, as the buttons do.
  *Done (1.22.1).*

## Music: what Sonos and Spotify could do next (October 2026, 1.27.4)

Ideas for the Sonos and Spotify screens, from a look at what the two APIs
offer that the screens don't use yet. Roughly in order of value for the
work.

- [ ] **83. Sonos sleep timer.** "Slå av om 15/30/60 min" on the Sonos
  screen: `AVTransport ConfigureSleepTimer`, run by the speaker itself, so
  it works with the display off or on another screen.
- [x] **84. Shuffle, repeat and seeking.** Buttons for shuffle and repeat
  (off, all, one) beside the skip buttons, and a tap on the progress bar
  to jump there, on both screens: Sonos `SetPlayMode` and `Seek
  REL_TIME` (a queue only: not radio, TV or AirPlay), Spotify
  `/me/player/shuffle`, `/repeat` and `/seek`. *Done: shuffle left of the
  skip buttons, repeat right (a "1" for one track), lit with a dot under;
  a knob on the progress bar where it can be wound. Tested on the device
  with a queue loaded silently and the play mode and place set from
  outside; host tests for the play mode, `can_seek` and Spotify's
  `disallows`.*
- [ ] **85. The music screen when music starts.** Jump to the Sonos or
  Spotify screen when playback starts, stay there while it plays, and go
  back to the rotation when it stops (or after a while paused). A setting,
  off by default; the Sonos screen would need to look at the rooms while
  not on show.
- [ ] **86. Volume per room in a Sonos group.** A row of - and + per room
  (`RenderingControl SetVolume` on each member) besides the group's.
- [x] **87. Radio in the Sonos picker.** NRK's channels as a page of the
  "Velg musikk" grid, started as plain streams (`x-rincon-mp3radio:` with
  the addresses on NRK's stream server), so they need no Sonos favourite or radio
  service. *Done: "Radio" beside "Velg musikk" opens the picker on the
  channels chosen on the setup page (Sonos, "Radiokanaler i Velg musikk"; up
  to 12 of 28: NRK's, the 15 most listened to others at myTuner, and
  P24-7 Juleradioen), each
  with its logo from the update site (bruk/radio/), also shown as the cover
  while it plays.*
- [x] **88. "Velg musikk" on the Spotify screen.** The account's playlists
  and what it played lately as a grid, as on the Sonos screen, started on
  the device playing (`PUT /me/player/play` with `context_uri`), or else
  the one that played last. *Done: up to 6 played lately, then the
  playlists; the grid is shared with the Sonos screen
  (`main/music_tiles.c`), and the playlists' names remembered for both in
  `spotify_client.c`.*
- [ ] **89. Mute.** `SetGroupMute` on Sonos; on Spotify the volume to 0 and
  back.
- [ ] **90. Sonos playlists in the picker.** The household's Sonos
  playlists (`SQ:`) after the favourites.
- [x] **91. "Kommer nå".** The next tracks in the queue (Sonos `Browse
  Q:0`, a tap jumps there with `Seek TRACK_NR`; Spotify
  `/me/player/queue`).
  Done: a **Kø** tab in both pickers, eight tracks from the one playing. On
  Sonos a tap plays that track (`Seek TRACK_NR`, the queue started first if
  the room played something else); on Spotify, which has no way to jump, a
  tap skips forward that many tracks.
- [ ] **92. Move Spotify to another device while it plays.** The device
  buttons are only shown while nothing plays.
- [ ] **93. Like a Spotify track.** A heart for the track playing; needs
  the `user-library-modify` permission, so everyone logs in again.
- [ ] **94. Soundbar settings.** Night sound and speech enhancement for a
  Sonos soundbar (`RenderingControl SetEQ` NightMode / DialogLevel), only
  for a room that has them.
- [ ] **95. The next Sonos alarm on the clock screen** (`AlarmClock
  ListAlarms`).
- [ ] **96. "Grupper alle"** on the Sonos screen: every room into the group
  playing in one tap.
- [x] **97. Events instead of polling.** Sonos speakers push changes to a
  subscriber (UPnP GENA): instant updates and less traffic than asking
  every 3 s, but subscriptions must be renewed and made again when an
  address changes. *Done in 1.32.0 (`main/sonos_events.c`): the group's
  AVTransport and GroupRenderingControl, a NOTIFY waking the poll, which
  then runs every 30 s besides; polling as before if subscribing fails.
  The NOTIFY bodies aren't parsed - what plays is asked for as before.*
- [ ] **98. "Spiller nå" on the other screens.** A thin bar with the title
  playing, a tap opens the player; optional, as it costs drawing on every
  screen.
- [ ] **99. Home Assistant's media players** (Chromecast, AirPlay and the
  rest) through Home Assistant rather than each protocol, with item 40.
- [x] **100. The music pickers' memory and speed** (review of 2026-10-21).
  A picture that fails holds no tile buffer (12 failed radio logos held
  ~260 KB the watchdog counted as free); the tiles' staging arrays are
  allocated only while used and a tile keeps a hash of its picture's
  address, not a copy (48 KB less PSRAM for good); the radio logos' sheet
  gets its 96 KB at once instead of doubling to 128 KB (192 KB for a
  moment); a shorter list frees the buffers it no longer needs. Spotify's
  picker asks only as many "spilt nylig" pages as get tiles, says "Henter
  musikken …" till they come, and starts on the device that played last
  only if it is still listed. Sonos asks the queue's play mode every 30 s
  (or after a change from the display) rather than every poll; a word
  tile isn't measured when it isn't drawn.
- [x] **102. A cover that needs less memory, and comes back.** A picture
  is collected into a buffer sized from the reply's Content-Length
  (`http_get_buf`) instead of doubling: a speaker's 180 KB AirPlay cover
  took 256 KB, and 384 KB for a moment, and failed with ~310 KB free. A
  cover that failed is asked for again after 30 s rather than never for
  that track (Sonos and Spotify).
- [x] **101. The radio logos kept between visits.** The radio page fetches
  `tiles.bin` (74 KB, HTTPS) each time it opens, so the logos take a
  moment; it could be kept (as JPEGs, not pixels) while the Sonos screen
  shows, at that much PSRAM. *Done in 1.32.0: kept by `music_tiles.c`
  till the screen is left (`mgrid_sheet_release`); one that failed is
  fetched again next time.*
- [x] **103. Volume set by a tap on the bar.** Today a tap anywhere left of
  the bar's middle lowers the volume a step (3 % on Sonos, 10 % on
  Spotify), so 12 % to 40 % on Sonos is ten taps. A tap on the bar should
  set the volume there, as on the progress bar, with - and + only at their
  buttons; holding - or + could repeat.
  Done on both screens; holding - or + to repeat is not (the screens get
  taps, not long presses).
- [x] **104. What plays marked in the picker.** A ring on the radio
  channel, favourite or playlist playing now (`s_now.radio`, the
  favourite's or context's URI), so the picker also shows what is on.
  Done: an accent ring (orange on Sonos, green on Spotify). On Sonos a
  favourite is matched by its address or its station name, and a playlist
  or album by what was last started from the display while its group plays
  from the queue (Sonos keeps no trace of where a queue came from).
- [x] **105. An answer the moment a tile is tapped.** The player keeps
  showing the old track until the speaker or Spotify reports the new one
  (1-3 s, longer for Spotify). Show the tile's name and picture at once
  with "Starter …"; on Spotify with nothing playing, also where it starts
  ("Spiller på Stue-TV"), as the device is chosen without asking.
  Done: the tile's name and picture (scaled up) with "Starter …" (Spotify:
  "Starter på <device> …"), till the speaker or Spotify reports something
  new, an error, or 10 s; polled every 1-1.5 s meanwhile.
- [ ] **106. Picking another Sonos room while music plays.** A tap on an
  idle room then adds it to the group; there is no way to show an idle room
  to start something else there. A long press (or a tap on the room shown
  in the group) could select instead of grouping, with a short note saying
  which happened ("Badet er med i gruppen").
- [x] **107. The picker closed by itself.** The Sonos picker stays open
  across visits (the rotation comes back to it hours later), while
  Spotify's is closed on every visit. Close both after a couple of minutes
  without a touch, and on entering the screen, so it shows what plays.
  Done: after 2 minutes without a touch while something plays, and on
  entering either screen.
- [x] **108. More than 12 in the Sonos picker.** Favourites and "spilt
  nylig" share 12 tiles, so recently played often doesn't show at all. A
  "Flere" tile, or a page per kind (Favoritter, Spilt nylig, Radio).
  Done: a tab per kind (Favoritter, Nylig, Lister, Radio, Kø) in place of
  the room buttons while picking, the room in the title; each list fetched
  when its tab is shown. Each page still holds 12 tiles.
- [x] **109. Spotify's playlists in the Sonos picker.** The Spotify screen
  offers the account's playlists; the Sonos picker only what was played
  lately. They could be a page of their own there, started through Sonos
  as the recent ones are.
  Done: the **Lister** tab, started through Sonos as the recent ones are.

Not possible: lyrics, and Spotify's audio features and recommendations,
which Spotify has closed to new apps; Chromecast and AirPlay directly are
too heavy for the chip.

## The ISS screen (October 2026, 1.30.0)

- [x] **110. Where the ISS is.** A screen with the station over a world
  map, its last orbit as a line and the next as dots.
  Done: orbit from Celestrak's TLE every 6 hours, SGP4 on the device
  (checked against python-sgp4), the visibility ring, day and night on the
  map, and the location chosen under **Sted på kartet**.
- [x] **111. The next pass over the chosen location.** When the station
  next rises over it, how high it goes, and whether it can be seen (the
  sky dark there, the station in sunlight) - the elements and SGP4 are
  already on the device. *Done in 1.35.0: bottom right on the map, the
  next pass above 10° with its times, height and why it can't be seen if
  it can't, and then the next that can, up to 4 days ahead
  (`main/iss_pass.c`, matched against Skyfield to within 3 s).*
- [x] **112. Forecast parsing within the PSRAM left.** After hours of use a
  weather screen had ~350 KB of PSRAM free, MET's complete forecast
  needed ~370 KB to parse, and every forecast failed and was fetched again
  each minute (the picture jumped and tore meanwhile). *Done in 1.30.2: the
  numbers the display doesn't read are left out before parsing
  (`drop_unused_numbers`, ~110 KB less), and a failed forecast waits
  3 minutes before the next try.*
- [x] **113. Free PSRAM sinks over the day.** On a weather screen it was
  ~470 KB just after a restart and ~350 KB seven hours later (the ISS map
  was checked and is given back). *Found: the rain screen's hour of frames
  (180 KB to 1 MB, sized to the PSRAM free) stayed allocated after the
  first visit. Now given back 15 minutes after the latest frame (when it
  can't be shown again anyway), or at once when PSRAM is short - see 115.*
- [x] **114. The swipe to the menu on the music screens.** On Spotify and
  Sonos a swipe up from the bottom edge seldom opened the screen menu: the
  volume row and the controls' column reach the edge and took the press
  first. *Done: a press on the bottom edge is checked for the swipe before
  the screen's own buttons, and given to them on release if it wasn't one.*
- [x] **115. Memory held by screens not on show.** Every configured screen
  kept its buffers from startup: the radar ~970 KB (coastline, water and
  rain images, water seeds), a satellite view 80-360 KB. *Done in 1.30.4:
  the radar takes its images on its first poll and gives them back when
  another kind of screen shows, and the rain image is four strips, so no
  500 KB block is needed in one piece. Kept data that can be fetched
  again - the rain frames and the satellite views - is given back when
  free PSRAM falls under 512 KB while their screen isn't on show
  (`TRIM_PSRAM`). A weather screen went from ~470 KB free to ~1.6 MB on
  the development display. The satellite views aren't given back
  otherwise: the image is 1.1 MB to fetch again, and a rain frame 190 KB.*
- [x] **116. Birthdays and events in the settings backup.** The display's
  own events (`/hendelser`, NVS namespace `hendelser`) weren't in
  `config.json`, so a new display or an erased one started without them.
  *Done in 1.31.0: the list `events` in the backup; a restore replaces the
  events with the file's, and an older file without the list keeps them.
  The upload limit for a restore went from 16 KB to 64 KB.*
- [x] **117. Passed events filling the list.** Appointments and other
  one-date events stayed after their day until deleted by hand, each
  taking one of the 100 places. *Done in 1.31.1: when an event is added,
  those whose day ended 30 days or more before are deleted first
  (`own_prune`). Only on adding, not on a timer, so the history stays
  until there's a new event and nothing changes the list on its own.*
- [x] **118. Sonos rooms left out of the row.** The room buttons kept
  their full names and padding, so with four rooms and "Radio" and "Velg
  musikk" the fourth didn't fit and wasn't shown. *Done in 1.31.1: the
  padding shrinks (18 to 14 px) and then the longest names are cut to the
  same width with ".." (`chip_layout`), not below 60 px.*
- [x] **119. Repeated appointments.** Appointments and other events were
  on one date only. *Done in 1.32.0: weekly (the same weekday, every 1-52
  weeks) or monthly (the same day, every 1-12 months; the 29th-31st on the
  last day of shorter months) from their date, never pruned. Two new
  fields in `own_event_t` (`repeat`, `every`), so the NVS key went from
  `v1` to `v2`; `v1` is read as once and erased on the next save. In the
  backup as `"repeat"` and `"every"` (left out for 1).*
- [x] **120. Replacement buses hidden from the train rows.** At Nittedal
  on 2 November 2026 the R31 row toward Oslo (`/v530`, via Kjelsås) showed
  one train, then none: Vy's replacement buses run as R31 with the same
  "Oslo S" sign but by Sinsen, so they failed the via filter - after
  taking the two places per line and destination that Entur gives
  (`numberOfDeparturesPerLineAndDestinationDisplay`), which left the next
  train out too. *Done in 1.32.1: a call whose journey is
  `railReplacementBus` is shown in the train's row, its time in orange,
  and the row's notice starts "Buss for tog". For a via direction it
  counts as going that way if a later stop of its is within three quarters
  of this stop's distance to the via stop (their coordinates now in the
  query). The row's track comes from the first train, not a bus.*
- [x] **121. Radio that can't pause.** Sonos plays a radio stream with
  Stop only, decided by the address's kind, so a proxy behind a custom
  radio station in the Sonos app would not help. *Done in 1.33.0: with an
  `http://` update address the radio page plays a channel as a plain file
  from the update site's new `radio` container (`/radio/KEY.mp3`, 2 GB
  long), which Sonos can pause; the container records the station while a
  listening is paused (an hour by default) and answers the speaker's
  range on Play from the recording. `scripts/sonos_radio_playlist.py`
  saves the channels as the Sonos playlist "Radio med pause" for the app.
  The station's titles (ICY), which Sonos reads only from a radio
  stream, are taken out by the container and given to the display for
  where the speaker is (`/radio/KEY.json`), as is how far behind it is,
  with the artist and a picture where the station sends them (P4 Group's
  JSON, Bauer's planetradio details), made 240 px by the container;
  the next button skips 30 s on and "Til direkte" to now. The Sonos app
  shows only the station's name.*
- [x] **122. No way to tell of an error from the display.** A user who
  saw something wrong had to describe it by mail, without the screen or
  the log. *Done in 1.34.0: the navigation page's "Meld fra om feil"
  takes a screenshot, asks what is wrong and sends both, with `/status`
  and `/log`, to the update site's new `feil` container, which keeps
  them under `server/feilmeldinger/` and pushes each to an ntfy topic
  with the screenshot. Rate, size and count capped, as it is reachable
  through the tunnel; reviewed for abuse: the sender taken from the
  tunnel's header only through the tunnel, two at a time and ten a day
  per sender, the screenshot decoded and made anew, a deep status JSON
  (which made the writing fail and left a half-written folder) kept as
  text, control characters taken out, the container on a network of its
  own with nginx.*
- [x] **123. A report can't be told from one a display sent.** Any web
  page could POST to `/feil/` (CORS was open for the navigation page) with
  any name and version, so pushes could be faked and the store filled
  from many addresses. A key in the firmware would not help: the images
  are public on the update site. *Done in 1.34.0: each display makes its
  own ECDSA P-256 key and signs the report, which it now sends itself
  (the browser gives it the screenshot and text); the site takes only
  signed, recent reports not seen before, without CORS, and at most ten
  new keys a day. Keys aren't approved beforehand (by choice): this stops
  random abuse, not a sender who makes keys of their own.*

## Review of November 2026 (1.35.0)

Three reviews of the firmware - reliability, security, and a daily user
and maintainer (`docs/reviews/`). Fixed by area in the order below, the
most important first.

### The web server

- [x] **124. A long header gets past the Host and cross-site checks.** A
  `Host` of 97+ characters or an `Origin`/`Referer` of 129+ doesn't fit
  the buffer, and `httpd_req_get_hdr_value_str`'s "truncated" was taken as
  "no header", which lets the request through. That undid #54 (DNS
  rebinding could read `/config.json?hemmeligheter=1`) and #26 (any
  http:// page with a long host name could POST to `/save`, `/config.json`
  or `/ota`) when no setup password is set.
  *Done in 1.36.0: a header that can't be read is refused, not taken as
  missing (`web_auth_host_hdr_ok`, `web_auth_same_origin`, host tested
  with cut headers); Origin and Referer are read into 256 bytes.*
- [x] **125. The fallback portal gives away the WiFi password.** When the
  saved network doesn't answer at boot, the open AP serves
  `/config.json?hemmeligheter=1` to anyone who joins. A phone left
  connected also stops the retries of the saved network for good.
  *Done in 1.36.0: `?hemmeligheter=1` is refused on the portal's network
  unless BOOT was held, and the saved network is tried every 10 minutes
  even with someone on the portal. The AP itself stays open (a key would
  have to be typed from the screen).*
- [x] **126. Script on the setup page through the update address.** The
  Spotify redirect address, made from the update address, was printed into
  the page unescaped; an address set through `/save` or a restored backup
  could carry a script that runs when the owner opens `/oppsett`.
  *Done in 1.36.0: escaped as every other address (and `'` too), and an
  update address with characters outside RFC 3986 is refused on `/save`,
  in a restored backup and on load (`app_config_ota_url_valid`).*
- [x] **127. GET requests that change things.** `/screen?vis=` and
  `/spotify/login` work from an `<img>` on any page, with no cross-site
  check.
  *Done in 1.36.0: both refused when the browser says the request came
  from another site (`Sec-Fetch-Site`, `web_auth_fetch_site_ok`); curl and
  scripts, which don't send it, still switch screens with GET.*
- [x] **128. The web server takes every socket.** lwIP has 10, the server
  may hold 7 plus 3 of its own and never closes idle connections, so a
  setup page or nav tab left open can leave no socket for the fetches
  ("Ingen internett").
  *Done in 1.36.0: 16 sockets in lwIP, the server at 5 clients, and TCP
  keep-alive (30 s, then 3 × 5 s) closes those whose browser is gone.
  Checked with 12 idle connections held: the forecast was still fetched.*
- [x] **129. One client can stall the web server.** An error report holds
  the single server task while it is sent on (up to 30 s), and Sonos
  events time out meanwhile.
  *Done in 1.36.0: the report is read, sent and answered from its own task
  (`httpd_req_async_handler_begin`), so the server goes on serving. A
  client that sends its headers a byte at a time can still hold the task
  (ESP-IDF has no deadline for the whole request).*
- [x] **130. Handlers registered while the server serves.** About 18
  handlers are added after `httpd_start`, without a lock, while the server
  task may walk the same table; a request in that window can crash it, and
  on a new release's first boot that rolls the release back.
  *Done in 1.36.0: every handler is registered on the server's own task
  through `httpd_queue_work` (`add_handler`).*
- [x] **131. An error report's screenshot holds most of the PSRAM.** Up to
  1.5 MB in one block while the report is sent, unknown to the memory
  watchdog, and a failed allocation was reported as "mottak".
  *Done in 1.36.0: refused with 507 and "ikke minne nok" on the
  navigation page unless 512 KB stays free in one piece beside it, and
  counted in `wd_psram_held` while it is held.*
- [x] **132. A wrong password slows everyone.** One shared pace for wrong
  passwords, so one host can keep the owner's logins at "Vent et sekund";
  the comparison also returns early on a different length.
  *Done in 1.36.0: the pace is per client address (the last 8,
  `web_auth_pace_t`), and the comparison takes the same time whatever the
  guess's length.*

### Time and dates

- [x] **133. The nightly restart around a DST change.** The next 02:00 was
  today's plus 24 h, so the October night had an extra restart (with the
  backlight on) and the March one was at 03:00.
  *Done in 1.36.0: the next 02:00 by the wall clock and calendar day
  (`nb_next_local`), host tested on both change nights: one restart a
  night, at 03:00 only on the March night that has no 02:00.*
- [x] **134. Date helpers copied and drifting.** `days_from_civil`, local
  midnight, the weekday and month names and "i dag / i morgen" texts exist
  five to eight times over; #133 came from one of the copies.
  *Done in 1.36.0: `components/nb_text.c` (host tested) holds the names,
  local midnight, the day number, "i dag / i morgen / i går" and the
  numbers; `civil_from_days` joined `days_from_civil` in `civil_time.h`,
  and the copies in `own_events.c`, `ical.c`, `holidays.c`, `sgp4.c` and
  the screens are gone. `events.c` keeps its HTML-escaped names.*
- [x] **135. The setup page's time parser has no host test**, and the
  backup's parser takes other forms ("7:5" but not "0730").
  *Done in 1.36.0: `form_hhmm` in `form_util.c`, with host tests, used by
  the form and by the backup restore.*
- [x] **136. Today's headings switch at the next poll, not at midnight**
  on the calendar and week screens.
  *Done in 1.36.0: both redraw from a 1 s timer when the local day
  changes.*

### The weather screens

- [x] **137. The week screen's first card is yesterday after midnight**
  until MET's forecast expires, and an old forecast is never marked.
  *Done in 1.36.0: days before today are skipped, and the label turns
  orange "Sist oppdatert" after `WX_STALE_S` as on the weather screen.*
- [x] **138. The overview never says its forecast is old.**
  *Done in 1.36.0: the oldest row's age top right, in orange, once it is
  older than `WX_STALE_S`.*
- [x] **139. Old data looks different on every screen**, and "Sist
  oppdatert kl. 14:02" doesn't say which day.
  *Done in 1.36.0: one `updated_label_set` (draw.c) on the weather, week,
  air, tide, calendar and departure screens: "Sist oppdatert" in orange
  with "i går", the weekday or the date when it isn't today
  (`nb_updated`); the departures keep their time in it rather than "viser
  siste data", and the rain radar's image time turns orange after 30 min.*
- [x] **140. A decimal point on some screens, a comma on others.**
  *Done in 1.36.0: `nb_dec` on the weather chart, the overview, the radar
  (heights, distances, rings) and the week screen.*
- [x] **141. "-0°", "-0.0°" and "+0 cm".**
  *Done in 1.36.0: `nb_round`/`nb_temp`/`nb_dec` round halves away from
  zero and never give -0; the tide's "Vær og vind" says "0 cm"; blue
  is for a temperature that reads below zero, on every screen.*
- [x] **142. "1.2–1.2 mm" when it rains now.**
  *Done in 1.36.0: `nb_mm_range` gives one amount when both ends are the
  same at a tenth.*
- [x] **143. The week screen's wind line can wrap** at the larger text
  sizes.
  *Done in 1.36.0: measured; " m/s", then the gust, is left out when it
  doesn't fit.*
- [x] **144. Small differences in Norwegian:** "kl 14" on the overview,
  no sign that its later columns are tomorrow, "tor 05:36" on the tide
  screen where the ISS screen says "i morgen".
  *Done in 1.36.0: "kl. 14", and "i morgen kl. 02" (or "i morgen 02"
  where narrow) on the first column of the next day; the tide screen says
  "i morgen 05:36" as the ISS screen does, the weekday after that.*

### The tide screen

- [x] **145. A tide level of "inf" or a huge number freezes the screen**
  (the grid loop never ends) and restarts the display in a loop.
  *Done in 1.36.0: levels that aren't finite or are 2000 cm or more are
  left out (`level`, host tested), and the grid has at most some ten
  lines and its loop a count.*

### The calendars

- [x] **146. The month calendar keeps a failed feed's events for ever**:
  the 12 h check used a stamp every partly successful fetch refreshes.
  *Done in 1.36.0: each calendar's last good read is kept, as on the
  calendar screen, and a failing one is named in orange under the title
  ("Kalender 2 ikke oppdatert").*
- [x] **147. Two calendar fetch loops with different rules** on the
  calendar and month screens.
  *Done in 1.36.0: both keep a failed calendar's events by one rule,
  `cal_keep_feeds` (ical.c, host tested), with a stamp per calendar. The
  fetches stay apart: each screen fetches only while on show, for its own
  window (two weeks, or 17 weeks around the month shown).*
- [x] **148. A timed event over several days says "til 14:00" today**
  when it ends days later, and isn't shown on the days after.
  *Done in 1.36.0: a timed event is listed under each day it covers, with
  its start, "til 14:00" on its last day, and "Hele dagen" between.*
- [x] **149. A calendar feed with old daily rules** is walked period by
  period from its start (a rule from 1900 is ~46,000 steps per event).
  *Done in 1.36.0: a rule without COUNT starts at the period that can
  reach the window (`first_period`), and one calendar walks at most
  400,000 periods; host tested with rules from 1900 (under 200 walked).*

### Sonos

- [x] **150. Any LAN device can answer as a Sonos speaker**, and decide
  the rooms and the cover addresses the display fetches.
  *Done in 1.36.0: once a speaker has answered, only speakers of its
  household (`X-RINCON-HOUSEHOLD`) are taken, and a cover is fetched over
  http:// only from the speaker, the update site or a public address
  (`sonos_art_allowed`, host tested).*

### The code

- [x] **151. `app_config_erase` is never called.**
  *Done in 1.36.0: removed.*
- [x] **152. No test that a backup restores every setting.**
  *Done in 1.36.0: `test_config.c` saves every setting through a backup
  with the secrets and compares the whole struct, with the fields left out
  on purpose named; checked to fail when one restore line is taken out.*
- [x] **153. Pure screen helpers without host tests** (departure times,
  the week screen's numbers, day texts, rain text).

  *Done in 1.36.0: the departure time and the nowcast's note are
  `nb_departure` and `nb_rain_change` (nb_text.c); the week screen's numbers,
  the day texts and the tide's times use nb_text too, all host tested.*
## Second pass (November 2026, 1.36.0)

What the 1.35.0 reviews didn't reach (docs/reviews/review-second-pass.md).

- [x] **154. Only the first four weather alerts are kept.** A red or
  orange alert after the fourth MetAlerts lists is dropped, and "(+N)"
  counts only the four. Keep the four most severe and count them all.
  *Done in 1.36.1: when full, a worse alert takes the mildest one's place,
  and those left out are counted in `more` for the "(+N)"; host tested with
  six alerts.*
- [x] **155. A Spotify artist line cut in the middle of a name**
  (`artists()` leaves the part snprintf wrote before the break).
  *Done in 1.36.1: the names that fit whole, the first cut at a whole
  character if it alone doesn't fit; host tested.*
- [x] **156. An air quality value of null is read as 0** (the best air)
  instead of the hour being skipped.
  *Done in 1.36.1: a value that doesn't start like a number is NAN, so the
  hour is skipped; host tested.*
- [x] **157. The watchdog can read a beat time half written** (64-bit on a
  32-bit CPU): a false restart, very unlikely.
  *Done in 1.36.1: the beats are 32-bit seconds since boot.*
- [x] **158. A stray comment from 1.36.0** above `save_form_into` in
  wifi_provision.c.
  *Done in 1.36.1: removed.*
- [x] **159. The radio proxy fetches any https picture a stream's metadata
  names**, up to 8 MB, from inside the home network. Only the stations'
  image hosts.
  *Done in 1.36.1: only https from P4's and Bauer's image hosts
  (`ART_HOSTS`, others logged and left out), and Pillow's pixel cap at 4096
  x 4096.*
- [x] **160. nginx lets 192.168.0.0/16 reach /radio/**, which Docker may
  also give a compose network: allow the home /24 only.
  *Done in 1.36.1: a request with Cloudflare's CF-Connecting-IP is refused
  first, whatever address the tunnel has; checked: 200 from the LAN, 403
  with the header.*

## Static analysis (November 2026, 1.36.1)

clang-tidy (with the Clang Static Analyzer), cppcheck, flawfinder and
semgrep over the display's code (docs/reviews/static-analysis.md).

- [x] **161. A long Spotify Retry-After restarts the display.** The weather
  task waits the whole Retry-After without a beat, so anything over 20
  minutes trips the watchdog, and the display restarts and asks again every
  20 minutes; past 4.29 million seconds the wait wraps. Keep the deadline in
  spotify_client and wait a few minutes at a time.
  *Done in 1.36.2: spotify_client keeps the deadline (Retry-After read with
  strtol, 1 s to a day) and answers SPOTIFY_ERR_RATE without asking until it
  has passed; the screen waits at most 5 minutes at a time.*
- [x] **162. The ship service's `expires_in` isn't clamped** (undefined
  behaviour over ~2.1 million seconds), and a token length check after it
  can never be true.
  *Done in 1.36.2: clamped to a minute to a day (NaN counts as a minute),
  the dead check removed; ships still fetched with the 3600 s token.*
- [x] **163. `app_config_from_json` writes `*n_events` without a NULL
  check** when `events` is given (no caller does that yet).
  *Done in 1.36.2: checked; host tested with an event in the backup (seen to
  crash under ASan without the check).*
- [x] **164. Tidying:** a dead store in `show_login_status`, and
  `raw_ac_t`'s unused `lat`/`lon` values.
  *Done in 1.36.2: `show_login_status` sets its message in every case (a
  `default` with the connected one's), and the unused fields are gone.*
- [ ] **165. Run the static analysis again** before the next release with
  many changes, or by May 2027 at the latest: the same four tools as in
  docs/reviews/static-analysis.md ("Running them again"), and compare with
  what that found. Newer versions find more, and new code hasn't been
  through them.

## From error reports

- [x] **166. Two temperatures on top of each other at the start of the
  weather chart** (report 20261106-061730-abde42, Nittedal): "now" (-3,6°)
  was labelled as well as the low an hour later (-3,7°), as one low with two
  numbers.
  *Done in 1.36.3: "now" gets no label when it only falls (rises) into a
  labelled trough (peak), the global one or a prominent one, at most 2 hours
  on and 1 degree off, so only the lowest (highest) is shown; further off,
  both are. Checked on the reported shape and at and past each limit.*
- [x] **167. The setup page's names for the two weather screens** ("Vær"
  and "Uke") don't say how far ahead they look, and "Satellitt" for a
  place's satellite picture.
  *Done in 1.36.4: "Vær neste to dager", "Vær neste 7 dager" and
  "Satellittbilde" (as the display-wide one is called); a place's
  summary line keeps the short names ("vær, uke, ..."), and "i automatisk
  bytte" goes on its own line where it doesn't fit beside the longer name
  (checked at 320, 375 and 800 px).*
- [x] **168. The clock screen's outdoor temperature** doesn't say how cold
  it feels in the wind.
  *Done in 1.36.5: "føles som -3°" beside it, smaller and dimmed, when
  MET's apparent temperature in the nowcast (Yr's "Føles som") is at least
  a degree off (rounding alone put "-2°" beside a calm "-1,5°"); without it,
  the wind chill index from the nowcast's wind (10 °C and below, at least
  4.8 km/h). The
  line narrows its gaps and then drops to 52 px digits to fit: the widest
  case ("Inne 28°", "Ute -25,5°", "føles som -38°") is 748 px of 768.*
- [x] **169. A screen for the family's own pictures**, from a microSD card
  in the display, added from the navigation page or from an address.
  *Done in 1.37.0: the Bilder screen (main/pictures.c) shows the card's
  pictures at random, each for 10 s to 15 min; the /bilder page shrinks
  every picture in the browser to fit 800x480 before sending, so the
  display holds one 750 KB picture while on show and nothing otherwise. The
  setup page greys the screen out without a card. The SPI driver's
  interrupt handler went to flash (CONFIG_SPI_MASTER_ISR_IN_IRAM off): in
  IRAM it took ~9 KB of internal RAM, leaving +2.3 KB of static DIRAM in
  all. Tested without a card on the display, and the page's shrinking (a
  4000x3000 photo to 640x480, filled 800x480, a portrait to 360x480)
  against a mock in Chromium, then with a 64 GB card. Cards are read as
  exFAT as well as FAT32/FAT16, so they work as they come (components/fatfs
  builds ESP-IDF's FatFs with exFAT on, which it has no setting for), with
  long file names in UTF-8: +11.5 KB code, nothing more of internal RAM.
  GPT partition tables are read too (FF_LBA64), which the first real
  card, 64 GB, needed; a card that answers but can't be mounted is reported
  as unreadable, with what is on it in the log. Tried with that card: an
  upload saved, read back byte for byte, and deleted. Each picture added
  is shown as the screen will show it, to choose the whole picture or
  filling the screen with the part chosen by dragging and zooming (slider,
  wheel, two fingers); tested in Chromium against a mock (a zoomed-in
  corner of a test picture came out as that corner, 800x480). On the
  display only the smallest picture showed: reading a file asked for it
  plus 384 KB in one PSRAM block, and with the 750 KB screen buffer taken
  the largest block was ~470 KB; now it leaves 128 KB beside it and 256 KB
  in all, and all three pictures on the card take their turn. A review of
  the new code then found and fixed: the list of screens one entry too
  short with every screen on (STOPS_GLOBAL 8 -> 9); the picture screen
  impossible to turn off without a card (a box that is on can now be
  cleared); the card held while a picture went to a slow browser, and
  while a picture was decoded, with the LVGL lock too (now read, let go,
  then decoded with no lock); the list's sizes found by a stat() per file,
  reading the folder n times (now FatFs's f_readdir, one pass); the picture
  on show able to come straight back after an upload; one bad file
  blanking a good picture; the decoder's size read uninitialised; the
  card's mutex made inside a critical section (now sd_card_init() at
  start); and /bilder/hent passing anything starting "BM" or with "ftyp"
  (MP4, MOV) - BMP's header and the HEIC/AVIF brands are now checked,
  host-tested in pic_util.*
- [x] **170. Downloads kept on the memory card**, when there is one, so
  they needn't be fetched again; the card stays optional.
  *Done in 1.38.0: components/sd_cache.c keeps the rain radar's
  frames (as the crop's rain levels, keyed by area, image time and crop)
  and covers and logos (Spotify's, the Sonos screen's from the queue and
  our radios', the pickers' tiles, the radio logo sheet) in CACHE/<kind>
  on the card, each file with its key, date, length and CRC-32, written as
  .TMP and renamed; oldest removed past a cap (48 files/16 MB of frames,
  1000 files/64 MB of covers). Without a card nothing changes; a failed
  mount isn't tried again by the cache for 5 minutes. On the display, after
  a restart the radar's hour came from the card in 3 s instead of 11 s and
  2 MB. Building the logo sheet into the firmware instead was considered
  and dropped: the firmware is run from PSRAM, so it would cost 74 KB of it
  all the time (see lessons-learned, "The firmware's size is PSRAM").
  Covers and the logo sheet then seen coming from the card on the display;
  not yet tried: taking the card out with the cache in use. A review then
  found and fixed: the card's logo sheet deleted before a fresh one was
  fetched (so with the site down, no logos at all), and fetched on every
  visit if a channel was in neither (now kept on failure, once a day at
  most); a tile not on the card looked for there on every poll, and again
  by the network fetch (now remembered for the visit); oldest-written
  removed first however much used (a read now dates the file anew); a card
  swapped while the picture screen held it kept the old card's counts and
  "full" (now by a mount count); counts that only grew (replaced and stale
  files now subtracted); without a card, a mount tried every 5 minutes
  for good (now doubling to an hour); the update site's radio logos kept
  30 days against the sheet's 7 (now 7); Sonos favourites' covers of
  streams from the speaker kept, which follow what plays
  (sonos_art_stable, host-tested); and the radio page's logos fetched
  twice per poll.*
- [x] **171. The radio logos shown one by one, and an empty page after
  Radio.** *Fixed in 1.38.0: the picker redrew the whole screen
  after each logo (~100 ms of software rendering, which the next waited
  for); logos from the sheet, and every cover the memory card has, are now
  all decoded first and drawn with one redraw (the lists' covers came
  three per poll, torn), and a cover fetched over the network redraws
  only its tile. The radio page's
  logos are fetched before the poll asks the speakers anything. Going from
  Radio (or Kø) back to the music page last shown left it empty until its
  lists changed (set_page emptied it, and the poll only builds a page that
  changed or isn't the one built); it is now built again.*
