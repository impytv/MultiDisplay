| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# Coded with Claude based on the LVGL9 Adapter Demo

This project targets the Waveshare `ESP32-S3-Touch-LCD-4.3B` board and runs the official
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

Flash through the board's native USB port, the one that shows up as
Espressif `303a:1001`. The other port goes through a CH343 serial chip
(`1a86:55d3`), which appears as a port but passes no data, so esptool stops
with "No serial data received". If the native port doesn't show up, hold
BOOT while plugging it in to start the board in download mode.

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
  **Installer ny programvare automatisk** (off by default). **Sjekk kl.**
  and **Og deretter hver (timer)** set when the display checks: at that
  local time and every so many hours from it (03:30 every 24 hours by
  default; 6 hours from 03:30 checks at 03:30, 09:30, 15:30 and 21:30).
  When ticked, a newer release is installed at those checks. **Sjekk
  nå** shows what the site offers, with its release notes, and **Installer
  nå** installs it straight away, ticked or not.
- **What is refused:** a manifest for another project or board, a version
  that isn't newer than the running one, an image whose version, size or
  SHA-256 differs from the manifest, and any image not signed with the
  project's key. A release that went back to the previous firmware after
  installing isn't installed automatically again; a newer one is.
- **Test releases:** point a display at `manifest-test.json` instead to get
  releases before the others.

The site is static files served by nginx in a Docker container on the
Raspberry Pi (`server/compose.yaml`), on port 8070. Once:

```bash
sudo apt install docker.io docker-compose
sudo usermod -aG docker $USER        # then log in again
sudo mkdir -p /srv/multidisplay && sudo chown $USER: /srv/multidisplay
cd server && docker compose up -d
```

The container mounts `/srv/multidisplay` read-only and starts again by
itself after a restart.

To reach the site from outside the LAN as well, over HTTPS through a
Cloudflare Tunnel (no port opened on the router):

1. In the Cloudflare dashboard, under Zero Trust → Networks → Tunnels,
   create a tunnel of type Cloudflared and copy its token.
2. Give the tunnel a public hostname, for example
   `oppdatering.example.no`, with service `http://update-site:8080`.
3. `cp server/.env.example server/.env`, put the token in it, and run
   `docker compose up -d` in `server/` again.

Displays then use `https://oppdatering.example.no/manifest.json` as their
update address. The firmware doesn't check the server's certificate (see
`sdkconfig.defaults`); the image signature is what protects an update.

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
    the chip's temperature (`brikketemperatur`, and `innetemperatur` with
    the clock screen's offset added), and each service's last success and error. The setup page shows it under
    **Vedlikehold → Driftsstatus**.
  - `/log`: the last 16 KB of the log.
  - `/coredump`: the crash dump from the last crash, if any. Decode it with
    `idf.py coredump-info -c coredump.bin`, using the build of that same
    firmware. `curl -X POST http://DEVICE-IP/coredump/erase` removes it.
    The crash dump partition comes with the partition table.
  - `/screen.png`: a screenshot of what's on the screen.
  - `/screen`: the screens a tap cycles through, numbered, and which one is
    on show. `/screen?vis=9` or `/screen?sted=2&type=tidevann` switches to
    one as a tap would (types: `oversikt`, `kalender`, `maned`, `klokke`, `vaer`, `uke`, `fly`,
    `skip`, `nedbor`, `avganger`, `luft`, `tidevann`, `europa`, `satellitt`;
    `sted` counts from 1),
    so `/screen.png` can capture it. Refused unless **Tillat å velge skjerm
    over nettet** is ticked under **Vedlikehold → Skjermbilder** (off by
    default; per display, so not in settings backups).
- **Name:** **Navn på skjermen** under **Vedlikehold** on the setup page (default
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
  the settings as a file and restore it, e.g. onto a new board. The WiFi
  network, its password and the BarentsWatch secret are left out unless
  **Ta med WiFi og BarentsWatch-hemmelighet** is ticked (the file then holds
  them in plain text); the setup page's password and the calendar addresses
  are never in it. Restoring keeps whatever the file doesn't have and
  restarts the display. Also `curl -O http://DEVICE-IP/config.json` (add
  `?hemmeligheter=1` for the secrets) and
  `curl --data-binary @multidisplay-innstillinger.json http://DEVICE-IP/config.json`.
- **Passwords:** each password field on the setup page has **Vis passordet**
  to show what's typed, to check it before saving.
- **Offline:** "Ingen WiFi" or "Ingen internett" shows bottom left while the
  display is off the network or every fetch has failed for two minutes. A
  screen showing older data after a failed fetch has its info line in
  orange.
- The setup page only accepts changes sent from itself, so another web page
  open on the home network can't change the settings.

- The project keeps the existing `4.3B` RGB, CH422G and GT911 bring-up flow, and only replaces the LVGL porting layer with `esp_lvgl_adapter`.
- The default panel resolution is `800x480`.
- Touch is enabled by default. If your panel variant has no touch, set `BOARD_TOUCH_GT911` to `0` in `components/waveshare_rgb_lcd_port.h`.

## YR weather (MET Norway)

The "YR" tab connects to WiFi and shows the current forecast from the
[MET Norway Locationforecast API](https://developer.yr.no/doc/), refreshed
every 10 minutes. As MET's terms ask, a forecast isn't asked for again until
its `Expires` time, and then with `If-Modified-Since`, so an unchanged
forecast costs nothing to check. The detail screen has the same large
24-hour clock as the departure board, top right, and when the forecast was
fetched at the bottom right. Left of the clock it shows today's sunrise and
sunset ("Sol 07:15–18:58", or Midnattssol / Mørketid), worked out on the device,
and the night hours are shaded in the charts. When the nowcast sees
precipitation starting or stopping within the next hour and a half, that
takes the sun times' place in blue: "Nedbør om 25 min", "Opphold om 10 min"
or "Nedbør den neste timen".

Hours worth looking up for the northern lights get a green bar down from the
top of the chart, marked **Nordlys**, as long as Yr's aurora value for the
hour (a value of 1 reaches the bottom), so the bars grow and shrink with the
forecast intensity as in the Yr app. An hour is shown when it is dark, the
sky at most 75 % clouded, and an aurora value of at least 0.3 in Yr's aurora
forecast for the place (about Kp 3 in Tromsø; Oslo needs a strong storm).
That forecast is Yr's own website API
(`www.yr.no/api/v0/locations/LAT,LON/auroraforecast`, what yr.no shows), not
a documented one, so it may change without notice; the chart then just shows
no aurora. Fetched hourly, as its cache headers allow.

To add a location, type a place name under **Finn sted** in a location block
on the setup page: the hits come from Kartverket's place names, and picking
one fills in the name, latitude and longitude.

### Multiple locations

Up to 5 forecast locations can be stored. **Tap the right half of the screen**
for the next stop and **the left half** for the previous one (both wrap
around). **Swipe up from the bottom edge** for a menu of every screen,
grouped by location like the navigation page: a button switches to that
screen, and a tap beside the buttons, a swipe down or half a minute
untouched closes it. **Slå av skjermen** at its top switches the screen
off until the next touch, which only lights it again (also through the
night window and the morning); while it's off, nothing is fetched and the
rotation stands still, as at night. A tap in the bottom 40 px acts when the finger lifts
rather than when it lands, to tell it from a swipe. The swipe can be turned
off with **Sveip opp fra bunnen for å velge skjerm** under **Vedlikehold →
Navigasjonsside**. The screens, in order:

1. **Oversikt** – an overview table with one row per location that shows
   weather and 6-hour columns, each showing the weather icon, temperature and
   the precipitation summed over that 6-hour block. Shown unless **Vis
   oversikten** under **Andre skjermer → Oversikt** on the setup page is unticked (its **i
   automatisk bytte** puts it in the rotation).
2. **Kalender**, if shown (see [Calendar](#calendar)).
3. **Månedskalender**, if shown (see [Month calendar](#month-calendar)).
4. **Klokke**, if shown (see [Clock](#clock)).
5. **Satellittbilde over Europa**, if shown (see [Satellite](#satellite)).
6. For each location in order, whichever of its screens are enabled.

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
route is known, with the column then headed **Type** or **Rute/type**),
altitude (metres, or kilometres from 1000 m), ground speed in knots (left
out at large font sizes) and distance in kilometres. Aircraft on the ground
are left out.
Police, rescue and ambulance aircraft show **Politi**, **Redning** or
**Ambulanse** (Pol., Redn., Amb. under **Til**) in place of the route, and
get their callsign tag on the plot before the others. They are known by
callsign where the service has its own (the Air Force's rescue helicopters
"SAVER..", ambulances "DOC..", "BNO..", "DFL.."), and for Norwegian
aircraft by the operator or owner in the Civil Aviation Authority's
[aircraft register](https://www.luftfartstilsynet.no/aktorer/norges-luftfartoyregister/registrerte-luftfartoy/)
(owned by the Police Directorate, or an air ambulance operator), looked up
two registrations per poll. Each callsign's route and each registration's
owner is looked up once and the answer kept while the display runs, also
when nothing was found; only a lookup that got no answer is tried again.
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

The plot can be centred somewhere other than the location, for a place
where the location itself would show mostly land (Oslo, say): search for a
place under **Midtpunkt for skip (valgfritt)**, or type its coordinates. The
screen is then titled after that place. Left empty, the ships are around
the location.

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
stored as `components/coast.bin` (about 2.1 MB) in its own `coast` flash
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
MET gives every alert whose area covers the location's coordinates and that
hasn't ended - in force now, or issued for later. When a location has one
or more:

- Its weather detail screen shows the worst one at the top, between the
  location's name and the clock, with the area it's meant for
  (`Snø – Fjelloverganger i deler av Troms og Finnmark`, `(+N)` if
  there's more than one), coloured by MET's own yellow/orange/red severity
  scale. A long one takes two lines.
- A bar in the same colour along the top of the temperature chart runs from
  when each alert comes into force to when it ends, so an alert for
  tomorrow evening sits over tomorrow evening.
- The overview table shows a small dot of that same colour next to the
  location's name.

The area matters: MET's regions are large, and the text often narrows an
alert down to mountain passes or high ground ("Deler av Sør-Norge" for snow
above 500 m), which a location in the valley inside the region still gets.
An alert held on the device stops showing once it has ended, even before
the next fetch.

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
below; orange above zero, blue below), the day's precipitation with the
high end of MET's range when that is more ("0,4–2,1 mm"), and its
strongest wind with the strongest gust in brackets ("5 (12) m/s"; MET
forecasts gusts for the first two days or so).

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
**Andre skjermer → Kalender** on the setup page, tick **Vis kalenderen** (and **i automatisk
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

### Month calendar

One screen for the whole display, after the calendar: under **Andre skjermer →
Månedskalender** on the setup page, tick **Vis månedskalenderen** (and **i
automatisk bytte** for the rotation). Five weeks from the Monday of this one
as a grid, Monday first, with the week number (ISO 8601) on the left, the
months shown in the title, and the month's short name on the first of each
month. Sundays and public holidays have their date in red, holidays with
their name under it; today is outlined, and days already past are faded.
The buttons by the clock step four weeks back or ahead (the last week shown
becomes the first) and **I dag** goes back to this week, as does leaving the
screen or five minutes without a touch.

Two options under it, both on unless unticked:

- **Månefaser** draws the moon's phase beside the date on the day of each new
  moon, first quarter, full moon and last quarter (as seen from the north:
  waxing on the right).
- **Andre hendelser på himmelen** names, under the date and any holiday:
  equinoxes and solstices, supermoons (full moons closer than 360 000 km),
  the peaks of the main meteor showers (Quadrantids, Lyrids, Perseids,
  Orionids, Leonids, Geminids; on the evening the night of the peak
  begins), and the eclipses that can be seen from the first location - a
  lunar eclipse with an umbral phase when the moon is up for it, and a solar
  eclipse with how much of the sun is covered there ("83 % sol-formørkelse").
  When a day has more than fits, eclipses come first.

And one off unless ticked:

- **Avtaler fra kalenderne** lists, under the date and any holiday, the
  events from the calendar screen's calendars (**Kalender**): a line each
  with a bar in the calendar's colour, the title cut with ".." to fit, an
  all-day event on each day it covers and others on the day they start.
  When there is no room for all, the last line says how many more ("+5");
  the events come before the sky's. They are fetched every 15 minutes for
  the five weeks on show, the four before and the eight after, so a step
  of the buttons shows them at once; a step further fetches at once.

The events are built in for 2026-2035 by `scripts/build_astro.py`, which
computes them with [Skyfield](https://rhodesmill.org/skyfield/) and JPL's
DE440s ephemeris (download `de440s.bsp` and pass its path). A solar eclipse
is stored as the sun's and moon's positions every 10 minutes; the display
shifts the moon for parallax at its own location and works out how much of
the sun's disc it covers while the sun is up (for 12 August 2026 in Oslo,
83 %, as Skyfield gets).

The holidays are built in, read from [norskkalender.no](https://www.norskkalender.no/)
by `python3 scripts/build_holidays.py` into `components/holidays_data.h`.
The site has them up to 2029; after that only Sundays are red until the
script is run again and the firmware rebuilt.

### Clock

One screen for the whole display, after the month calendar: under **Andre
skjermer → Klokke** on the setup page, tick **Vis klokken** (and **i
automatisk bytte**). The time across the middle in large digits (the
seconds smaller beside them), the date and ISO week number under it, and
the temperature indoors in whole degrees and outdoors to the nearest half
degree ("Inne 21°   Ute 6,5°";
the setup page shows the indoor tenths, for calibrating). The indoor
temperature comes from the chip's own sensor and can be well off even
calibrated; untick **Vis temperaturen inne** to leave it out (in backups
as `clock.indoor`).

The outdoor temperature is MET's nowcast for the location chosen under
**Temperatur ute for** (the first unless changed; **Ingen** shows indoors
only, and nothing is fetched). The choice follows its location as the
locations are moved, and falls back to **Ingen** if it is removed. It is
in backups as `clock.outdoor_location` (1 = the first, 0 = none). The
nowcast gives the temperature now, which MET corrects with the
observations around it (Netatmo's private stations among them,
quality-controlled against each other and the model), so it is better
than averaging nearby stations ourselves - tried, and in sunshine many
private sensors read 2-5 °C warm.
Fetched every 10 minutes while the clock is shown; "Ute –" after half an
hour without an answer. With the rain radar down ("temporarily
unavailable") the nowcast still has the temperature.

The temperature comes from the ESP32-S3's own sensor (`components/chip_temp.c`),
read every 5 s and smoothed over about a minute. It measures the chip,
which runs some 15 °C above the room, so an offset is added: type what a
thermometer by the display shows into **Riktig temperatur nå (°C)** and the
offset is worked out from the chip's reading when the page is saved (a
room's temperature, 0 to 40 °C, and only once the chip has a reading;
anything else is refused and the offset kept), or type the offset itself into **Justering (°C)**. Calibrate after the display
has been on for half an hour. The offset belongs to that board's sensor, so
it is left out of settings backups. **Vis temperaturen inne** is off
until the display is calibrated: ticking it needs a thermometer reading in
the same save, and a backup turns it on only on a display that already has
an offset. The chip's warmth follows how hard it
works too (fetching, the rain radar), so expect the reading to wander by a
degree or so.

### Spotify

One screen for the whole display, after the clock: what the display's
Spotify account is playing, with the cover, the title, the artists and
the album, a progress bar, buttons for the previous track, play/pause and
the next track, shuffle and repeat (off, the playlist or album, the track:
"1"), the volume (- and + in steps of 10 %, or a tap on the bar sets it there) and the device it plays on;
a tap on the progress bar winds there (`/me/player/shuffle`, `/repeat`,
`/seek`; greyed out where Spotify's `actions.disallows` says so, never for
an ad). With nothing playing, a button for each of the account's
devices starts playback there. **Velg musikk** beside the title swaps the
player for tabs: **Nylig**, up to 12 tiles of what the account played
lately (the playlist or album, as on the Sonos screen); **Lister**, its
playlists (`/me/playlists`); and **Kø**, what plays and what comes
(`/me/player/queue`), where a tap skips forward to that track (Spotify
can't jump in the queue: `next` that many times). A tile starts its
playlist or album (`PUT /me/player/play` with `context_uri`) on the device
playing, or else the one that played last if it is still listed, or else
the first that takes commands; the player shows its name and picture with
"Starter på <device> …" at once, till Spotify plays it. The tile of the
context playing is ringed. Each page's list is asked for when it is shown
(again after 2 minutes); the picker closes by itself after 2 minutes
without a touch, and on every visit. The covers are held only while the
grid is on show, and the player's cover is given back meanwhile (and the
tiles while the player shows). The grid
is shared with the Sonos screen (`main/music_tiles.c`). The display plays nothing itself; it
controls Spotify on a phone, speaker or computer through Spotify's Web
API, and controlling needs **Spotify Premium**. Some devices (many TVs,
amplifiers and Cast speakers) are *restricted*: Spotify refuses commands
for them ("403 Restricted device"), so their controls are greyed out with a
note - also for a device that refuses without being marked so. Asked every 5 s while
playing (10 s paused), at once after a button; the progress bar moves on
by itself between, and the poll comes early when a track should have
ended. The buttons are drawn and hit-tested by `main/spotify.c`
(`spotify_tap`, like the month calendar's); a tap elsewhere changes screen
as usual.

Setting it up:

1. Create an app at <https://developer.spotify.com/dashboard> (its owner
   needs Premium): tick **Web API**, and add the **Redirect URI** the setup
   page shows - the folder `spotify/` next to the manifest, which must be
   `https://` (Spotify takes plain http only for 127.0.0.1). Under **User
   Management** add the name and e-mail of each Spotify account to be
   used; an app in development mode allows five. One app serves every
   display.
2. On the setup page, under **Andre skjermer → Spotify**, paste the app's
   **Klient-ID**, tick **Vis Spotify** (and **i automatisk bytte**), and
   save. Unticking **Automatisk bytte kan starte herfra** keeps the
   rotation from starting while the Spotify screen is shown (one already
   running passes through as usual); **Alltid mørkt** draws this screen in
   the dark theme's colours whatever the theme.
3. Press **Koble til Spotify** there and log in with the account this
   display is to control.

The login uses Authorization Code with PKCE, which needs no client secret
(`components/spotify_client.c`). `/spotify/login` sends the browser to
Spotify with a fresh verifier and a state holding a nonce and the display's
address as the browser reached it. Spotify returns to the update site's
`server/spotify/index.html`, which sends the browser on to
`http://<display>/spotify/svar` with the code; that page asks for no
password (the login cookie is SameSite=Strict, so it doesn't come along
from another site), and the state, made for this login and used once, is
the check. The weather task then trades the code for the tokens and asks
for the account's name, shown on the setup page. The refresh token is kept
in NVS (namespace `spotify`) for this display only - not in backups, and
dropped if the client ID changes; **Koble fra Spotify** forgets it. The
client ID and the two ticks are in backups as `spotify.client_id`,
`spotify.show` and `spotify.rotate`. The login also asks for
`user-read-recently-played` and `playlist-read-private`, for the Sonos
screen's "recently played".

### Sonos

One screen for the whole display, after Spotify: what plays in a room of
Sonos speakers on the local network, with the cover, the title, the
artist and the album, a progress bar, buttons for the previous track,
play/pause (stop for a radio, which can't pause) and the next track, and
the volume (- and +, in steps of 3 %). Playing from the room's queue (not
a radio, TV, line-in or AirPlay), there are shuffle and repeat buttons too
(`SetPlayMode`, read with `GetTransportSettings`), and a tap on the
progress bar winds there (`Seek REL_TIME`). A row of buttons at the top has a
button per room (a stereo pair or a sub isn't a room of its own); the rooms
playing together, as grouped in the Sonos app, are lit. While something
plays, a tap on an idle room adds it to the group, in sync (`x-rincon:` to
the group's coordinator), and a tap on a lit room takes it out and stops
it there (`BecomeCoordinatorOfStandaloneGroup`; if it led, the others play
on). A room playing something of its own has a dot, and a tap only shows
it, so nobody's music is taken over; so does any tap while nothing plays,
or in the grid. Playback and the volume go to the group (- and + a step of
3 %, or a tap on the bar sets it there). **Velg musikk** swaps the
player for tabs in place of the rooms (the room it plays in is shown in
the title): **Favoritter**, the household's Sonos favourites; **Nylig**,
what the display's Spotify account played lately, as the playlist or
album each track was played from (from an artist, Liked Songs or a search,
the track's album); **Lister**, the account's playlists (those two with a
green dot, and only while Spotify is connected); **Radio**; and **Kø**, the
queue from the track playing (`Browse Q:0`), where a tap plays that track
(`Seek TRACK_NR`, the queue started first if something else played). A
tile starts it in the room, replacing the room's queue; the player shows
its name and picture with "Starter …" at once, till the speaker reports
something new. What plays is ringed: a radio channel, a favourite by its
address or station name, or what was last started from here while its
group plays from the queue. Each page's list is fetched when it is shown
(again after 2 minutes). A room with nothing to play opens the picker by
itself; it closes by itself after 2 minutes without a touch, and on every
visit. **Radio** beside it opens the picker on the radio channels chosen on
the setup page: out
of 28, NRK's twelve and the sixteen other stations most listened to at
[myTuner](https://mytuner-radio.com/no/radio/land/norway-stasjoner) (P4,
Radio Norge, P5 Hits, P10 Country, P7 Klem, P11 Dance, NRJ, Radio Rock, P9
Retro, Radio Vinyl, P8 Pop, P6 Rock, Topp 40, P12 Hitmix, P24-7 Mix, and
P24-7 Juleradioen's Christmas music). They
start as plain MP3 streams (`x-rincon-mp3radio://`, addresses from
[radio-browser.info](https://www.radio-browser.info/); NRK's at its stream
server, as lyd.nrk.no's send on to them through a relative redirect, which
Sonos doesn't follow), so they need no favourite or radio service in the
Sonos app. Each has its logo on its tile and as the cover while it plays
(also in the Sonos app, as the stream's album art), fetched from the update
site's `bruk/radio/` beside the guide: `tiles.bin`, all of them at tile
size in one file for one request, and `KEY.jpg` at 240 px for the cover.
`scripts/build_radio_logos.py` makes them from each station's myTuner page.
They aren't built into the firmware: it runs from PSRAM, so its every byte
costs as much of that. Without the update site, the tiles have the
channel's name instead, as has any other radio without a cover of its own
in the cover square.

No login and no cloud: the speakers are found with an SSDP search, and
asked through the UPnP control they answer on port 1400 - as Home
Assistant uses them (`components/sonos_client.c`): the rooms from
`ZoneGroupTopology`, what plays from `AVTransport`, the volume from
`GroupRenderingControl`, the favourites from `ContentDirectory` (`FV:2`).
Spotify items are queued the way the Sonos app shares them: Spotify's URI
in an `x-rincon-cpcontainer:` address with Spotify's Sonos service in the
metadata (2311 in most places, 3079 in some; the one Sonos takes is kept).
This needs Spotify added in the Sonos app, and works for playlists -
Spotify's own mixes too - albums and tracks, not for artists. Sonos
Radio's favourites have no address of their own and can only be started
from the Sonos app (Sonos's own API leaves them out too), so they aren't
shown. "Recently played" needs the Spotify screen's login, though not the
screen itself; playlists are named by asking Spotify, which doesn't name
its own playlists for apps in development mode ("Spilleliste" then).
Asked every 3 s while playing (6 s otherwise), at once after a button
(taps are queued, each for the group it was meant for); the rooms and
groups every 30 s, and at once after a room joined or left, or when the
room stops answering. The
covers are held only while the screen is on show (as the Spotify screen's
are), the player's not while the grid is, and decoded scaled down: a
speaker's covers are 640 px. Covers may be JPEG or PNG (Spotify's
"Liked Songs" is a PNG; read a row at a time, not interlaced ones). Spotify is asked for recently played 10
tracks a page, up to 40 (50 at once is ~150 KB of reply), and the
connection is closed again until the grid's next refresh.

Under **Andre skjermer → Sonos** on the setup page: **Vis Sonos** (and
**i automatisk bytte**), **Automatisk bytte kan starte herfra** and
**Alltid mørkt** as for Spotify, and **Rom som vises først** (empty: the
first room that plays, else the first by name; the rooms found are offered
once the screen has looked), and **Radiokanaler i Velg musikk**, a box per
channel, NRK's and the others, for the picker's radio page: at most 12 (its
tiles), shown in the setup page's order (at first NRK P1, P2, P3, mP3 and
Nyheter, P4, Radio Norge, P5 Hits, P10 Country, P7 Klem, NRJ and Radio
Rock; with none, the picker has no **Radio** button). The display must be on
the same network as the speakers. In backups as `sonos.show`,
`sonos.rotate`, `sonos.start_rotation`, `sonos.dark`, `sonos.room` and
`sonos.radios` (the chosen channels' keys, comma separated, as in
`RADIOS` in `components/sonos_parse.c`).

### Satellite

MET Norway's latest Meteosat image of Europe (a new one every 15 minutes),
under **Andre skjermer → Satellittbilde** on the setup page:

- **Vis Europa** (and **i automatisk bytte**) adds one screen with the whole
  image, after the calendar.
- **Satellitt** under a location adds a close-up around it, with a ring on
  the location. **Forstørrelse i nærbildene** blows it up 3 times (about
  2000 km across) or 2 times (about 3200 km, sharper); an image pixel is
  about 8 km, so more would only be blur.
- The infrared image is shown day and night. With **Synlig lys når det er
  lyst**, the visible-light image is shown instead while the sun is at least
  5° up over all of Europe (if that screen is shown) and at every location
  with a close-up - one kind for all of them, as each fetch fills every view.

The image is decoded as it streams in, so the ~1 MB file is never held;
Europe takes 360 KB of PSRAM (256 colours, a palette per kind of image fitted
by `scripts/build_sat_palette.py`), each close-up 80 KB (180 KB at 2 times). MET
doesn't document the image's projection: `scripts/fit_satellite.py` fitted
one to the coastline drawn on it (within about a pixel over Norway); run it
again if MET changes the image.

### Night

Under **Visning → Natt** on the setup page, **Nattmodus** with its **Fra**/**Til**
times (local time, default 22:00–07:00) either dims the screen (**Demp
skjermen**: a dark layer over it, as the backlight can't be dimmed) or
switches it off (**Slå av skjermen**). When it's off, a touch lights it for
**Lyser etter trykk** (1, 2, 5, 10, 15 or 30 minutes, default 2; in backups
as `night.wake_minutes`); so does picking a screen over the web. That touch
only wakes it.
While it's off, the automatic rotation stands still and nothing is fetched
at all - only the clock keeps itself set by NTP, and the firmware update
check is still made at its time. Lighting it (in the morning, or by a touch)
refreshes the screen on show at once. The one exception: firmware installed
while the screen is off keeps fetching until something has worked, as that
is what proves the update good (else the nightly restart would roll it
back).

When a fetch fails, the air quality, pollen and satellite image are asked
for again once after a short wait (30 s, 1 min), then only every 5 minutes;
with a satellite image still on show, after 5 minutes from the first.

### Public transport departures

Tick **Avganger** for a location to give it a departure board: realtime
departures from [Entur's Journey Planner](https://developer.entur.no/pages-journeyplanner-journeyplanner),
the next two per line and direction,
under a large 24-hour clock. Refreshed every 30 seconds while on screen; the
"N min" countdowns tick between refreshes. Times without realtime data are
dimmed, cancelled departures say *Innstilt*. Trains show their track
("Oslo S (Spor 2)"); buses leave out their stop position ("Fornebu" rather
than "Fornebu (F)"). When the rows don't fit, they are first drawn closer
together, then in two columns. There the times say "11 m" rather than
"11 min" and destinations are cut short to fit.

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

Lines can also be gathered under headings of your own, across stops, such
as "Til jobb": an entry starting with `#` names them (`|` between names, at
most 6), and a line (with its direction) ending in `#<n>` goes under name
`n`. A line can be listed once per direction, each under its own heading:

```
#Til jobb|Hjem;4740=RUT:Line:385/in#1,RUT:Line:385/out#2,RUT:Line:390/in;58858=VYG:Line:R31/v530#1
```

The headings come first on the board, in their order, and the lines in
none stay under their stop's name. The text holds up to 399 characters.

You rarely need to write it by hand: below the field the setup page has a
departure picker (`components/setup_departures.js`, served as `/dep.js`). Search for a stop (nearest the location's coordinates
first), tick lines (none = all) and, for a line, a direction (none = both);
the field fills in as you go. Under **Grupper** you name the headings, and
each ticked line or direction then gets a choice of heading. For trains the picker works out the directions
from the order of the stops and names them by where the trains go. The picker calls Entur from the browser, so it
needs internet and doesn't work on the device's own setup WiFi.

### The navigation page

**`http://<name>.local/`** (`http://multidisplay.local/` unless renamed) has
a button for each screen the display has, grouped by location as a tap
steps through them, with the one on show marked (checked every 5 s, so taps
on the display show too). A press switches the display as a tap on it would:
the automatic rotation waits until the display has been left alone for its
idle time. Touching the display works as before. Under the screens,
**Slå av skjermen** switches the screen off as the screen menu's button does
(till a touch); while it's off the button reads **Skjermen er av - slå den
på** and lights it (as does pressing a screen). The setup page is a link
away, at **`/oppsett`**, and so is the usage guide (**Brukerveiledning**):
`bruk/` on the update site, next to the manifest of the display's update
address (`http://192.168.0.119:8070/bruk/` for the default), so a display
updating through the tunnel links to the guide there. **Vis navigasjonssiden** under **Vedlikehold →
Navigasjonsside** turns it off, and / is then the setup page (per display,
so not in settings backups).

### The setup page

At **`/oppsett`** (or / with the navigation page off, and always in the
setup portal). The locations come first, each a closed row showing its name and screens;
open one to change it. Only the fields its ticked screens need are shown,
**+ Legg til sted** adds one and **Fjern stedet** removes it. The rest is
in closed sections: **Visning** (theme, night, text size), **Automatisk
bytte**, **Tilgang og passord** (WiFi, setup password, BarentsWatch, yr
contact) and **Vedlikehold** (the display's name, the navigation page,
updates, firmware upload, backup, status). The overview, calendar and
satellite image have closed sections of their own under **Andre skjermer**.
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

Once connected, the same page is reachable at `http://<name>.local/oppsett`
(`http://multidisplay.local/oppsett` unless renamed) or the device's IP on your LAN
(in the router's client list, or the serial log: `Got IP: …`) for later
edits.

The page can have a password (**Passord for oppsettsiden**; none by default).
It covers both pages, the screenshot, the diagnostics and firmware updates.
In a browser, the pages ask for it once: the browser is then kept logged in
by a cookie for a year, or until the password changes or **Logg ut** on the
navigation page is pressed. The cookie holds an HMAC-SHA256 of the password
under a random key kept on the display, not the password, and scripts in
pages can't read it (HttpOnly) or send it from other sites (SameSite).
curl and scripts give it as HTTP Basic, with any user name
(`curl -u x:PASSWORD …`). It travels unencrypted, so don't reuse an
important one. After a wrong password the display checks no other for a
second (429 meanwhile).

The display only answers requests addressed to it: its IP, or its name
alone or as `.local`, `.lan`, `.home`, `.home.arpa` or `.localdomain`.
That stops another web site from reaching it by making its own domain
point at the display's address (DNS rebinding). Holding BOOT while powering on
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
