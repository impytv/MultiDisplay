# Second pass (November 2026, 1.36.0)

All seven fixed in 1.36.1 (docs/improvements.md, 154-160).

A look at what the three reviews of 1.35.0 didn't reach: the updater and
OTA writer, http_util, the Entur, rain, aircraft, air quality, MET alerts and
Spotify parsers, the PNG decoder and inflater, diag, the watchdog, the
screenshot, the Sonos event receiver, the firmware upload, the setup page's
scripts, and the server side (nginx, feil, radio). gcc `-fanalyzer` was run
over everything the host tests build; its five warnings were all false
positives.

Found sound, as far as read: the update chain (manifest checks, version
check against the image's own description, size, SHA-256, signature by
`esp_ota_end`, erase of the sector after the image, rollback), the PNG
decoder's bounds, the feil service's checks (signature before parsing,
replay, caps, the screenshot re-encoded), the setup page's HTML escaping and
the events page's.

## Findings

### 1. Only the first four weather alerts are kept (medium)

`met_alerts_parse` stops at `MET_ALERTS_MAX` (4) features, in the order
MetAlerts lists them. A red or orange alert after the fourth is dropped, so
the bar can show a yellow one while a worse one is in force, and "(+N)"
counts only the four kept. Five or more at once happens in storms (wind,
waves, flood, landslide, avalanche, ice for one county).
*Fix:* keep the four most severe (replace the mildest when full) and count
them all.

### 2. A Spotify artist line cut in the middle of a name (low)

`artists()` in spotify_parse.c breaks out of the loop when the next name
doesn't fit, but snprintf has already written the part that did:
"A, B, Lon" instead of "A, B", possibly ending in half a UTF-8 character.
*Fix:* put the terminator back at `p` before the break.

### 3. An air quality value of null is read as 0 (low, not seen)

`variable()` in air_client.c reads `"value": null` with strtof, which gives
0, not NAN: an hour without an AQI would show as the best air instead of
being skipped. Not seen from MET, but nothing stops it.
*Fix:* NAN unless the value starts like a number.

### 4. The watchdog can read a beat time half written (low)

`s_wd_lvgl_us` and `s_wd_weather_us` are 64-bit and written by one task,
read by the esp_timer task, on a 32-bit CPU. A read between the two halves
of a write that carries into the high word (once every 71 minutes) gives a
time 71 minutes old: a false "stalled" restart. The window is nanoseconds,
so it is very unlikely, but it costs one `atomic` or a critical section to
close.

### 5. A stray comment from 1.36.0 (cosmetic)

wifi_provision.c: `/* "HH:MM" (as an <input type=time> sends it) to minutes
after midnight ... */` is left above `save_form_into` now that `parse_hhmm`
moved to form_util.c.

### 6. The radio proxy fetches any https picture a stream names (low)

`song_of` takes P4's `imageUrl` from the stream's metadata if it starts with
https://, and `art()` fetches it (up to 8 MB) and decodes it with Pillow.
The station decides what the Pi fetches, from inside the home network.
*Fix:* only the hosts the stations use (NRK's and Bauer's image servers),
and `Image.MAX_IMAGE_PIXELS` set low.

### 7. nginx lets 192.168.0.0/16 reach /radio/ (low)

The comment says the tunnel is kept out because its requests come from
the container's address, but Docker hands out 192.168.x.0/20 networks too
once its 172.x pools are used up; a compose network there would let the
tunnel through. *Fix:* allow the home network's own /24 (and deny the
compose networks first).
