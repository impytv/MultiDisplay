| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# Coded with Claude based on the LVGL9 Adapter Demo

This example targets the Waveshare `ESP32-S3-Touch-LCD-4.3B` board and runs the official
`lv_demo_widgets()` demo with:

- `LVGL 9`
- `espressif/esp_lvgl_adapter`
- RGB panel output
- GT911 touch input

The Waveshare `ESP32-S3-Touch-LCD-7` is also supported with no code changes: it shares the
same 800x480 RGB timing, GPIO pinout, CH422G backlight/reset expander and GT911 touch wiring
as the 4.3B (confirmed against Waveshare's own [ESP-IDF LVGL9 example](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7/tree/main/examples/ESP-IDF/09_lvgl_v9_demo)),
so the same firmware image runs on either board unmodified.

## Requirements

- ESP-IDF `>= 5.5`
- Internet access on the first build so the component manager can download dependencies

## Build

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

The coastline map data is in its own partition and is not written by
`idf.py flash` (it takes over a minute). Write it once on a new board, and
again only after regenerating it:

```bash
idf.py -p PORT coast-flash
```

## Notes

- The example keeps the existing `4.3B` RGB, CH422G and GT911 bring-up flow, and only replaces the LVGL porting layer with `esp_lvgl_adapter`.
- The default panel resolution is `800x480`.
- Touch is enabled by default. If your panel variant has no touch, set `EXAMPLE_USE_TOUCH` to `0` in `main/waveshare_rgb_lcd_port.h`.

## YR weather (MET Norway)

The "YR" tab connects to WiFi and shows the current forecast from the
[MET Norway Locationforecast API](https://developer.yr.no/doc/), refreshed
every 10 minutes.

### Multiple locations

Up to 5 forecast locations can be stored. **Tap the right half of the screen**
for the next stop and **the left half** for the previous one (both wrap
around):

1. **Oversikt** – an overview table with one row per location that shows
   weather and 6-hour columns, each showing the weather icon, temperature and
   the precipitation summed over that 6-hour block, plus the device's IP
   address in the bottom-right corner (for reaching the setup portal later).
   This is always the first stop, even with only one location or a purely
   radar-only setup - it's the only screen showing the IP address, so it must
   always stay reachable by tap.
2. For each location in order, whichever of its weather screen (chart + wind),
   aircraft radar and ship traffic are enabled.

The setup page also has a **Theme** choice (light or dark) that applies to
every screen.

### Aircraft radar

For each location on the setup page, tick what it shows: **Weather**,
**Aircraft** and/or **Ships** (shown in that order: Oslo weather, Oslo
aircraft, Oslo ships, next location, ...). Each location also has its own
**Aircraft range** (kilometres, 10-185, default 40). A location without
weather has no weather screen and isn't a row in the overview table (the overview lists only the locations that show weather, and
is empty of rows - but still shown, for its IP address - if none do).

The screen shows a sonar-style plot centred on the location (north up, range
rings at quarter steps, a heading triangle and a 60-second speed vector per
aircraft, callsign tags for the nearest ones) and a table of the nearest 14
aircraft: callsign, type, altitude (metres, or kilometres from 1000 m), ground
speed in knots and distance in kilometres. Aircraft on the ground are left out.
Airports within range are drawn under the aircraft - runway lines, or a dot
where they'd be too small to see, plus the airport's IATA code (its ICAO code
where it has no IATA one; labels that would land on another label are
skipped). The table (about 8,500 airports and their runways: all large and
medium airports, scheduled small ones, and small ones with an ICAO code and a
paved 2000 ft runway) is embedded in flash as `components/airports.bin`;
regenerate it from the public-domain [OurAirports](https://ourairports.com/data/)
data with `python3 scripts/build_airports.py`.

Aircraft positions come from [adsb.fi's open API](https://opendata.adsb.fi/) -
free, no key, personal non-commercial use, one request per second at most - and
are polled every 5 s while a radar screen is showing (the connection is kept
open between polls, and no weather forecasts are fetched meanwhile), then
extrapolated along each aircraft's track in between.

### Ship traffic

Ships use the same screen layout as the aircraft radar: a plot centred on the
location with a hull-shaped marker along each ship's heading and a 10-minute
course vector (moored or anchored ships are plain dots), name tags for the
nearest ones, and a table of the nearest 14: name, type (Last, Tank, Pass,
Fiske, Fritid, Slep, Annet - also the marker colour), speed in knots and
distance in kilometres. Each location has its own **Ship range** (kilometres,
2-100, default 20).

Positions come from the [BarentsWatch Live AIS
API](https://developer.barentswatch.no/docs/AIS/live-ais-api), which needs a
free API client: create one at [BarentsWatch](https://www.barentswatch.no/minside/)
with access to AIS, and enter its **Client ID** and **Client secret** on the
setup page. The screen fetches the latest positions every 30 s (ships that
haven't reported for 15 minutes are left out) and moves moving ships along
their course in between. Coverage is Norwegian waters only, and small vessels
are filtered out by BarentsWatch (fishing boats under 15 m, leisure boats under
45 m).

Both the ship traffic and the aircraft radar plots show the coastline, from
[OpenStreetMap](https://www.openstreetmap.org/copyright) (© OpenStreetMap contributors, ODbL; credited on screen). The coast of Norway
and its neighbours (57.5-71.5° N, 4-31.5° E) is simplified to about 40 m and
stored as `components/coast.bin` (about 3.8 MB) in its own `coast` flash
partition; the tiles around a location are read and drawn once when its ship
or aircraft screen is shown, leaving out islands under a couple of pixels
across at that range. Regenerate it with `python3 scripts/build_coast.py`, which
downloads the processed OSM coastlines (about 900 MB) from
[osmdata.openstreetmap.de](https://osmdata.openstreetmap.de/data/coastlines.html).

The overview stop always exists and is always reachable by tap, regardless of
how many locations show weather. All locations that show weather have their
hourly forecasts kept refreshed in the background so the table is always
current; the detail screens additionally splice in the 5-minute nowcast for
the selected location.

### Severe weather alerts

Each location that shows weather is checked against [MET Norway's MetAlerts
API](https://api.met.no/weatherapi/metalerts/2.0/documentation) (the same
"farevarsel" warnings shown on yr.no), refreshed on its own 10-minute cadence.
When a location has one or more currently active alerts:

- Its weather detail screen shows the worst one's name at the top centre
  (`OBS: <name>`, `+N` if there's more than one), coloured by MET's own
  yellow/orange/red severity scale.
- The overview table shows a small dot of that same colour next to the
  location's name.

Nothing is shown for a location with no active alert. The API itself filters
to alerts covering the location's exact coordinates and currently active, so
the device does no date or geometry filtering of its own.

### Setup portal (WiFi + locations)

WiFi credentials and the forecast locations are configured at runtime and
stored in NVS — no rebuild needed to change them.

On a fresh flash (WiFi SSID still the `myssid` placeholder in `sdkconfig`),
or any time you **hold the BOOT button while powering on**, or if the saved
WiFi fails to connect, the device starts a setup access point:

1. Connect a phone/laptop to the WiFi network **`MultiDisplay-XXXX`** (open).
2. A "sign in to network" page opens automatically (captive portal); if not,
   browse to **`http://192.168.4.1/`**.
3. Pick your WiFi network, enter the password, then fill in one or more
   **Location** blocks (name + latitude/longitude). Leave a block empty to
   skip it. Press **Save** — the device reboots and connects.

Once connected, the same page is reachable at the device's IP on your LAN
(shown in the router's client list, or the serial log: `Got IP: …`) for
later edits.

### Build-time seed defaults

The values under `idf.py menuconfig` → `MultiDisplay Configuration` only
pre-fill the portal form (and let you skip the portal by setting a real
`WiFi SSID`). The **YR API User-Agent** is still build-time only and must be
set — api.met.no's
[Terms of Service](https://developer.yr.no/doc/TermsOfService/) reject a
missing/generic User-Agent with `403 Forbidden`; use something like
`MyDevice/1.0 myname@example.com`.

`sdkconfig` is **not committed** (see `.gitignore`); `sdkconfig.defaults`
holds the shared build settings and regenerates a fresh `sdkconfig` on first
build.

The "Fly" tab (aircraft radar) and severe weather alerts described above have
since shipped; a dedicated rain radar map ("Regn") remains a placeholder for
a future feature.
