# Ship traffic screen – implementation plan

A third per-location mode, alongside weather and the aircraft radar, that shows
live ship traffic from the BarentsWatch Live AIS API.

## BarentsWatch API

- **Token:** `POST https://id.barentswatch.no/connect/token`, form-encoded
  `grant_type=client_credentials`, `scope=ais`, `client_id`, `client_secret`.
  The reply holds `access_token` and `expires_in`.
- **Ships:** `POST https://live.ais.barentswatch.no/live/v1/latest/combined`
  with `Authorization: Bearer <token>` and a body like:

  ```json
  {"modelType":"Simple","modelFormat":"Json","since":"<now-15min>",
   "geometry":{"type":"Polygon","coordinates":[[[lon,lat],...]]}}
  ```

  Returns an array of `mmsi, name, latitude, longitude, speedOverGround,
  courseOverGround, trueHeading, shipType, navigationalStatus, msgtime`.
- **Coverage:** Norwegian waters only. Small vessels are filtered out (fishing
  boats under 15 m, leisure boats under 45 m).
- Docs: <https://developer.barentswatch.no/docs/AIS/live-ais-api>,
  OpenAPI: <https://live.ais.barentswatch.no/index.html>.

## 1. Configuration (`components/app_config.h/.c`)

- Add `APP_SHOW_SHIPS 0x04` to the per-location `show[]` bitmask. Existing
  stored settings keep working unchanged.
- Add a per-location `ship_km[]` range, 2–100 km, **default 20 km**, stored as
  a new NVS blob `"shipkms"`. Separate from the aircraft range.
- Add global `ais_client_id[128]` and `ais_client_secret[128]`, NVS keys
  `"aisid"` and `"aissec"`, shared by all locations.

## 2. Setup page (`components/wifi_provision.c`)

- Replace the Weather / Radar / Both dropdown with three checkboxes: Weather,
  Aircraft, Ships. If none is ticked, the location falls back to Weather.
- Add a "Ship range (km)" field per location.
- Add a "BarentsWatch" section with the client ID and a password-type client
  secret field, plus a note on where to register.
- Grow the page buffer from 12 KB to about 16 KB.

## 3. New component `components/ais_client.c/.h`

Modelled on `adsb_client`.

- **Token handling:** fetch, cache, and renew about 60 s before expiry. On a
  401 from the ship request, drop the token and retry once. URL-encode the ID
  and secret (client IDs contain `@` and `:`). The token connection is opened
  and closed each time.
- **Ship request:** a square search polygon around the location, sized to the
  range and adjusted for latitude. Client-side: drop ships outside the circle,
  compute distance and bearing, sort nearest first, keep the nearest 32
  (`AIS_MAX_SHIPS`).
- **Connection:** kept open between polls. At 30 s apart the server may close
  it; if so, reconnect once and carry on.
- **Buffer:** response in PSRAM, capped at about 256 KB (busy fjords can
  return hundreds of ships).
- **Result fields:** name, mmsi, distance, bearing, speed (kn), course,
  heading (511 = unknown, use course instead), type category, and a
  moored/anchored flag (navigation status 1 or 5, or speed under 0.5 kn).
- `since` needs a synced clock; SNTP already runs.

## 4. Display (`main/main.c`)

- **Stops:** in `build_stops()`, a location's screens run
  weather → aircraft → ships. Tap navigation needs no changes.
- **Reuse the radar screen:** same `s_radar_root` and canvas with a
  ships/aircraft mode flag, to save internal RAM; `radar_draw_cb` branches by
  mode.
- **Plot:** range rings and compass letters, no airports. A ship is an
  elongated triangle along its heading with a 10-minute projection vector.
  Moored ships are plain dots. Colours by type category (cargo, tanker,
  passenger, fishing, leisure, other), added to the light and dark radar
  palettes.
- **Table:** Navn / Type / kn / km, with the types Last, Tank, Pass, Fiske,
  Fritid, Slep, Annet.
- **Text:** title "Skip nær X", status "Henter skip...". Errors:
  "Mangler BarentsWatch-nøkkel" (no credentials) and "Innlogging feilet"
  (401 after retry).
- **Polling:** one API call every **30 s** (`SHIP_POLL_MS 30000`). Between
  calls, ships are moved along their course and the plot is redrawn every
  2 s, as on the aircraft radar. The header shows the data age, e.g.
  "oppdatert 12 s siden". Weather refreshes are skipped while the ship screen
  is shown.

## 5. Testing

- Check the token and ship request with `curl` using real credentials, to
  confirm the response size for the configured locations.
- Build, flash, check free internal memory in the log on the ship screen, and
  try both themes.

## Open question

- A simplified coastline (Kartverket or OSM) could be added to the plot later,
  stored in flash the same way the airport data is.
