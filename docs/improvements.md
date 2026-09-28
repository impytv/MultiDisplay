# Improvements

Findings from a review of the whole project (September 2026), most important
first within each group. Items are ticked off as they are implemented; the
planned order is 1, then 2 + 6, then 3, 7 and 10.

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
- [ ] **4. The nightly reboot masks leaks.** Log the minimum free internal heap
  and restart on a low threshold instead of relying only on the 02:00 reboot.

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
  indication of position or of the rotation running. Page dots, a rotation
  marker and long-press to the overview would help.
- [ ] **9. Rotation at night.** Rotation (and its fetching) continues while the
  screen is dimmed; an option to pause it at night.

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
