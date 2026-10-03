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

The firmware is signed, so a build needs the signing key in
`keys/ota_signing_key.pem`. It is kept out of git: anyone holding it can
make firmware the displays accept. Copy it from the build machine, or make a
new one for a new set of displays (those then need one USB flash to learn
it):

```bash
espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys/ota_signing_key.pem
```

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

### Updating over WiFi

After the first USB flash, new firmware can go over WiFi. Upload
`build/multi_display.bin` under **Vedlikehold → Last opp programvare** on the setup page, or:

```bash
curl --data-binary @build/multi_display.bin http://DEVICE-IP/ota
# with a setup page password:
curl -u x:PASSWORD --data-binary @build/multi_display.bin http://DEVICE-IP/ota
```

The display restarts on the new firmware. If that firmware doesn't get
through one full round of fetching and drawing, the next restart goes back
to the previous one. Only firmware built from this project and signed with
the project's key is accepted, but any version: this is also how to go back
to an older release. Changes to the partition table, the bootloader, the
fonts or the coastline still need USB, and so does a display whose firmware
was signed with a key that has since been lost.

### Automatic updates

The display can fetch new firmware by itself from an update site on the
home network (see `docs/auto-update-plan.md` for the design).

- **On the display:** the setup page's **Vedlikehold → Programvareoppdatering** section has the
  update address (default `http://192.168.0.119:8070/manifest.json`) and
  **Installer ny programvare automatisk** (off by default). When ticked, a
  newer release is installed at night between 03:30 and 05:00. **Sjekk
  nå** shows what the site offers, with its release notes, and **Installer
  nå** installs it straight away, ticked or not.
- **What is refused:** a manifest for another project or board, a version
  that isn't newer than the running one, an image whose version, size or
  SHA-256 differs from the manifest, and any image not signed with the
  project's key. A release that went back to the previous firmware after
  installing isn't installed automatically again; a newer one is.
- **Test releases:** point a display at `manifest-test.json` instead to get
  releases before the others.

The site is static files served by nginx on the Raspberry Pi. Once:

```bash
sudo apt install nginx
sudo mkdir -p /srv/multidisplay && sudo chown $USER: /srv/multidisplay
sudo cp server/nginx-multidisplay.conf /etc/nginx/sites-available/multidisplay
sudo ln -s /etc/nginx/sites-available/multidisplay /etc/nginx/sites-enabled/
sudo nginx -t && sudo systemctl reload nginx
```

To publish, raise the version in `version.txt`, commit, and run:

```bash
scripts/publish_firmware.py --channel test --notes "What changed"
scripts/publish_firmware.py --promote 1.1.0      # the test release -> stable
scripts/publish_firmware.py --channel stable --notes "What changed"   # straight to stable
```

It builds, checks the signature, copies the image to
`/srv/multidisplay/firmware/`, writes the manifest and the site's index
page, and tags the commit (`git push origin v1.1.0` to share the tag).

### Tests

`make -C test/host` builds and runs the host tests: the firmware's own
parsers and helpers (settings, backup, Entur, ADS-B, HTTP dates, form
parsing, sun times, versions) compiled with gcc against stub ESP-IDF headers,
under AddressSanitizer and UBSan. GitHub Actions runs them and builds the
firmware on every push (`.github/workflows/ci.yml`); that build is signed
with a throwaway key, so displays won't accept it.

## Notes

- A software watchdog restarts the device if the screen stops updating for a
  minute, the fetching stalls for 20 minutes, or memory stays low for a
  minute; the next boot logs which. The memory figures are logged hourly.

- **Diagnostics without the cable** (opening the serial port resets the
  board). On the device's address, behind the setup password if one is set:
  - `/status`: version, uptime, why it last restarted, WiFi signal, memory,
    and each service's last success and error. The setup page shows it under
    **Vedlikehold → Driftsstatus**.
  - `/log`: the last 16 KB of the log.
  - `/coredump`: the crash dump from the last crash, if any. Decode it with
    `idf.py coredump-info -c coredump.bin`, using the build of that same
    firmware. `curl -X POST http://DEVICE-IP/coredump/erase` removes it.
    The crash dump partition comes with the partition table; a board flashed
    before 1.1.0 needs `idf.py -p PORT partition-table-flash` once (it only
    adds a partition at the end).
- **Name:** **Navn på skjermen** at the top of the setup page (default
  `multidisplay`) is the display's address on the home network,
  **`http://<name>.local/`**, and the name the router lists it under. Give
  each display its own name to have several on one network. Only a-z, 0-9
  and '-' are used: "Kjøkken" becomes `kjokken`. The name belongs to the
  display, so settings backups leave it out.
- **Clock:** set by NTP - the router's server if its DHCP answer names one,
  then pool.ntp.org and time.cloudflare.com - or, until NTP answers, from
  the Date of the first response from MET. "Klokken er ikke stilt" shows
  bottom left if it still isn't set ten minutes after start.
- **Settings backup:** under **Vedlikehold → Sikkerhetskopi** on the setup page, download
  the settings as a file (everything except the WiFi network and the
  passwords/secrets) and restore it, e.g. onto a new board. Restoring keeps
  the display's own WiFi and secrets and restarts it. Also
  `curl -O http://DEVICE-IP/config.json` and
  `curl --data-binary @multidisplay-innstillinger.json http://DEVICE-IP/config.json`.
- **Offline:** "Ingen WiFi" or "Ingen internett" shows bottom left while the
  display is off the network or every fetch has failed for two minutes. A
  screen showing older data after a failed fetch has its info line in
  orange.
- The setup page only accepts changes sent from itself, so another web page
  open on the home network can't change the settings.

- The example keeps the existing `4.3B` RGB, CH422G and GT911 bring-up flow, and only replaces the LVGL porting layer with `esp_lvgl_adapter`.
- The default panel resolution is `800x480`.
- Touch is enabled by default. If your panel variant has no touch, set `EXAMPLE_USE_TOUCH` to `0` in `main/waveshare_rgb_lcd_port.h`.

## YR weather (MET Norway)

The "YR" tab connects to WiFi and shows the current forecast from the
[MET Norway Locationforecast API](https://developer.yr.no/doc/), refreshed
every 10 minutes. As MET's terms ask, a forecast isn't asked for again until
its `Expires` time, and then with `If-Modified-Since`, so an unchanged
forecast costs nothing to check. The header shows today's sunrise and sunset
("Sol 07:15–18:58", or Midnattssol / Mørketid), worked out on the device,
and the night hours are shaded in the charts. When the nowcast sees
precipitation starting or stopping within the next hour and a half, that
takes the sun times' place in blue: "Nedbør om 25 min", "Opphold om 10 min"
or "Nedbør den neste timen".

To add a location, type a place name under **Finn sted** in a location block
on the setup page: the hits come from Kartverket's place names, and picking
one fills in the name, latitude and longitude.

### Multiple locations

Up to 5 forecast locations can be stored. **Tap the right half of the screen**
for the next stop and **the left half** for the previous one (both wrap
around):

1. **Oversikt** – an overview table with one row per location that shows
   weather and 6-hour columns, each showing the weather icon, temperature and
   the precipitation summed over that 6-hour block. Shown unless **Vis
   oversikten** under **Oversikt** on the setup page is unticked (its **i
   automatisk bytte** puts it in the rotation).
2. **Kalender**, if shown (see [Calendar](#calendar)).
3. For each location in order, whichever of its screens are enabled.

The setup page also has a **Tema** choice (light or dark) that applies to
every screen.

**Kontakt-e-post for yr** goes into the User-Agent header of every api.met.no
request (`MultiDisplay/1.0 (<email>)`), as MET Norway's
[Terms of Service](https://developer.yr.no/doc/TermsOfService/) ask. Leave it
blank to send the build-time **YR API User-Agent** instead.

### Aircraft radar

For each location on the setup page, tick what it shows: **Vær**,
**Fly** and/or **Skip** (shown in that order: Oslo weather, Oslo
aircraft, Oslo ships, next location, ...). Ticking **Fly** shows its own
**Radius (km)** right under it (kilometres, 10-185, default 40); every
screen's settings sit under its checkboxes the same way. A location without
weather has no weather screen and isn't a row in the overview table (the overview lists only the locations that show weather, and
is empty of rows if none do).

The screen shows a sonar-style plot centred on the location (north up, range
rings at quarter steps, a heading triangle and a 60-second speed vector per
aircraft, callsign tags for the nearest ones) and a table of the nearest 14
aircraft: callsign, route (e.g. "OSL-BGO", from adsb.lol's route data,
or just the destination under **Til** at large font sizes; the type when no
route is known), altitude (metres, or kilometres from 1000 m), ground
speed in knots (left out at large font sizes) and distance in kilometres.
Aircraft on the ground are left out.
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
distance in kilometres. Each location has its own **Radius (km)** under **Skip** (kilometres,
2-100, default 20).

Positions come from the [BarentsWatch Live AIS
API](https://developer.barentswatch.no/docs/AIS/live-ais-api), which needs a
free API client: create one at [BarentsWatch](https://www.barentswatch.no/minside/)
with access to AIS, and enter its **Klient-ID** and **Klienthemmelighet** on the
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

### Automatic rotation

The setup page's **Automatisk bytte** section makes the display page
through screens on its own when nobody is using it. After **Etter (min uten trykk)** minutes without a touch it moves to the next screen ticked in a
location's **i automatisk bytte** boxes, next to its ticked screens (plus the overview and the
calendar, if theirs are ticked), in the normal screen order, and moves on every **Per skjerm
(s)** seconds. A touch stops it until the display has been left alone that
long again. 0 minutes (the default) turns it off, and nothing is ticked by
default. With **Stopp om natta** (on by default) it stands
still, and only that one screen fetches, while the night dimming is on.
Screens the rotation shows aren't remembered across a restart
(only tapped-to ones are), to spare the flash.

### The week ahead

Tick **Uke** for a location to get seven day cards from its forecast: the
weekday, MET's symbol for the middle of the day, the day's temperature
range as a bar on a scale shared by the week (the high above it, the low
below), the day's precipitation and its strongest wind.

### Air quality and pollen

Tick **Luft** for a location to get a screen with:

- **Air quality** from MET Norway's
  [airqualityforecast](https://api.met.no/weatherapi/airqualityforecast/0.1/documentation)
  (all of Norway): the level now in MET's colours (*Lite*, *Moderat*, *Høy*,
  *Svært høy* luftforurensning), the pollutant that drives it, and the next
  24 hours as one coloured cell per hour. Fetched every 30 minutes,
  honouring MET's `Expires`.
- **Pollen** for alder, birch, grass and mugwort from
  [Open-Meteo](https://open-meteo.com/en/docs/air-quality-api), which serves
  the European Copernicus (CAMS) model: the level now in NAAF's words
  (*Ingen*, *Beskjeden*, *Moderat*, *Kraftig*, *Ekstrem*, using NAAF's limits
  of 1/10/100/1000 grains per m³ for trees and 1/10/30/150 for grass and
  mugwort) and the next 24 hours as bars. It is a model forecast, so it can
  differ from NAAF's official pollen forecast (whose API needs an agreement
  with NAAF). Out of season it says *Ingen pollen i lufta nå*.

### Tides

Tick **Tidevann** for a location on the coast to get the water level from
Kartverket's [tide API](https://vannstand.kartverket.no/tideapi_no.html), for
the nearest permanent station (named above the chart): the level now and
whether it is rising or falling, how much the weather adds or takes away
(*Vær og vind*), the next four high and low tides, and a chart from six hours
ago to 30 hours ahead with Kartverket's forecast (tide and weather), the tide
alone and what the station measured. Heights are in cm over chart datum
(*sjøkartnull*). Fetched every 30 minutes. Inland there is no data, and the
screen says so.

### Calendar

One calendar screen for the whole display, right after the overview: under
**Kalender** on the setup page, tick **Vis kalenderen** (and **i automatisk
bytte** for the rotation) and paste up to three secret iCal addresses -
Google Calendar's *Hemmelig adresse i iCal-format*, a published Outlook
calendar's ICS link, or any `https://`/`webcal://` `.ics` address. The next
two weeks are shown as an agenda in two columns, a heading per day and a
coloured bar per calendar. Fetched every 15 minutes, read as it streams in,
so a calendar of several MB is fine; the server's certificate is checked.
Recurring events (daily, weekly, monthly, yearly, with exceptions and moved
instances) are expanded; times with a time zone are taken as Norwegian time.
The addresses are secret, so they are never shown on the page again (a blank
field keeps the saved one) and are left out of settings backups.

### Night

Under **Visning → Natt** on the setup page, **Nattmodus** with its **Fra**/**Til**
times (local time, default 22:00–07:00) either dims the screen (**Demp
skjermen**: a dark layer over it, as the backlight can't be dimmed) or
switches it off (**Slå av skjermen**). When it's off, a touch lights it for a
minute; that touch only wakes it.

### Public transport departures

Tick **Avganger** for a location to give it a departure board: realtime
departures from [Entur's Journey Planner](https://developer.entur.no/pages-journeyplanner-journeyplanner),
the next two per line and direction,
under a large 24-hour clock. Refreshed every 30 seconds while on screen; the
"N min" countdowns tick between refreshes. Times without realtime data are
dimmed, cancelled departures say *Innstilt*.

The stops and lines go in the location's **Avganger (holdeplasser og linjer)**
field as one line of text:

```
58366=RUT:Line:31/out,RUT:Line:25;6505;58858=VYG:Line:R31/v502
```

Stops are separated by `;`. Each is a stop place ID (a bare number means
`NSR:StopPlace:<number>`), optionally followed by `=` and the line IDs to
show, comma-separated; a line ID ending in `/in` or `/out` shows only that
direction, and a stop without lines shows all of them. Trains (Vy) have no
in/out direction in Entur's data, so their direction is `/v<stop>`: only the
departures that call at that stop place later on (`/v502`, Hakadal, keeps
the northbound trains at Nittedal; `+` joins more than one). At most 4 stops
and 8 lines per stop.

You rarely need to write it by hand: below the field the setup page has a
departure picker (`components/setup_departures.js`, served as `/dep.js`). Search for a stop (nearest the location's coordinates
first), tick lines (none = all) and, for a line, a direction (none = both);
the field fills in as you go. For trains the picker works out the directions
from the order of the stops and names them by where the trains go. The picker calls Entur from the browser, so it
needs internet and doesn't work on the device's own setup WiFi.

### The setup page

The locations come first, each a closed row showing its name and screens;
open one to change it. Only the fields its ticked screens need are shown,
**+ Legg til sted** adds one and **Fjern stedet** removes it. The rest is
in closed sections: **Visning** (theme, night, text size), **Automatisk
bytte**, **Tilgang og passord** (WiFi, setup password, BarentsWatch, yr
contact) and **Vedlikehold** (updates, firmware upload, backup, status).
Changes are marked "Ulagrede endringer" until saved, and mistakes are shown
by the field before anything is saved. Times are 24-hour (TT:MM). The page
follows the browser's light or dark mode.

### Setup portal (WiFi + locations)

WiFi credentials and the forecast locations are configured at runtime and
stored in NVS — no rebuild needed to change them.

On a fresh flash (WiFi SSID still the `myssid` placeholder in `sdkconfig`),
or any time you **hold the BOOT button while powering on**, or if the saved
WiFi fails to connect, the device starts a setup access point. If the saved
WiFi is only down for now (a router still starting after a power cut), the
device tries it again every minute, unless someone is connected to the setup
network, and restarts normally once it's back:

1. Connect a phone/laptop to the WiFi network **`MultiDisplay-XXXX`** (open).
2. A "sign in to network" page opens automatically (captive portal); if not,
   browse to **`http://192.168.4.1/`**.
3. Pick your WiFi network, enter the password, then fill in one or more
   **Sted** blocks (name + latitude/longitude). Leave a block empty to
   skip it. Press **Lagre og start på nytt** — the device reboots and connects.

Once connected, the same page is reachable at `http://<name>.local/`
(`http://multidisplay.local/` unless renamed) or the device's IP on your LAN
(in the router's client list, or the serial log: `Got IP: …`) for later
edits.

The page can have a password (**Passord for oppsettsiden**; none by default).
The browser then asks for it, with any user name; it covers the page, the
screenshot and firmware updates (`curl -u x:PASSWORD …`). It travels
unencrypted, so don't reuse an important one. Holding BOOT while powering on
opens the setup network without it. Saved passwords and secrets are never
shown in the page: leave a field blank to keep what's saved.

### Build-time seed defaults

The values under `idf.py menuconfig` → `MultiDisplay Configuration` only
pre-fill the portal form (and let you skip the portal by setting a real
`WiFi SSID`). The **YR API User-Agent** is the fallback used when no contact
email is set on the setup page — api.met.no's
[Terms of Service](https://developer.yr.no/doc/TermsOfService/) reject a
missing/generic User-Agent with `403 Forbidden`; use something like
`MyDevice/1.0 myname@example.com`.

`sdkconfig` is **not committed** (see `.gitignore`); `sdkconfig.defaults`
holds the shared build settings and regenerates a fresh `sdkconfig` on first
build.

The "Fly" tab (aircraft radar) and severe weather alerts described above have
since shipped; a dedicated rain radar map ("Regn") remains a placeholder for
a future feature.
