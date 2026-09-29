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
- [ ] **38. More screens from open Norwegian data.** *Air quality and pollen
  done (1.3.0): a "Luft" screen per location with MET's air quality
  forecast and CAMS pollen via Open-Meteo (NAAF's own API needs an
  agreement). Tides and a calendar remain.*
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

