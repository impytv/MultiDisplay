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
- [ ] **50. A failed aurora fetch waits an hour.** One transient failure at
  start-up leaves the weather screens without aurora marking for an hour.
  Retry a failure after 10 minutes; keep the hour for successes.
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
