# Lessons learned

From the work on firmware 1.0.0–1.3.0 (September 2026): automatic updates,
diagnostics, the Norwegian setup page, host tests and the air screen. What
went wrong, why, and what to do next time.

## Checks before calling a change done

- [ ] **Internal RAM:** compare `idf.py size` (the DIRAM line) with the
  previous release, and look at `/status` (free and lowest internal DRAM)
  after a few minutes on the device. Large static arrays get
  `EXT_RAM_BSS_ATTR` (PSRAM) from the start. A task stack in PSRAM only
  for tasks that never call OTA functions or memory-map flash. After moving
  anything to PSRAM, watch the rain radar for a few minutes (PSRAM
  bandwidth feeds the display).
- [ ] **Dependencies:** read the `dependencies.lock` diff after adding or
  changing a component, and pin anything the build is sensitive to.
- [ ] **Sequences, not just features:** test what people do in a row -
  above all "install an update, then save or restore settings within 20 s",
  and a power cut or restart in the middle of things.
- [ ] **Real settings:** test with the display's actual settings (21 px text,
  five locations, night mode on) as well as the defaults.
- [ ] **Restart reason:** after installing or restarting, `/status`
  should say "omstart fra programmet", not a watchdog.
- [ ] **Stacks:** when adding work to a task, check its stack size; anything
  that writes flash or logs needs more than 2 KB. When a struct grows,
  look for it as a local on a task stack.
- [ ] **The new version is running:** after installing, check the version
  in `/status` (and that there's no crash dump) before testing on it.
- [ ] **Widest content:** size text columns for the widest possible value
  ("WWW", "-88°"), not a typical one, measured with `draw_text_w` at the
  font size set; a `draw_text` that is too narrow wraps onto the next line,
  so text of unknown length goes through `draw_text_fit`.
- [ ] **Tests prove what they claim:** start from a clean state (erased slot,
  empty cache), and check that a "refused" case fails for the intended
  reason.
- [ ] **CI steps run locally first,** exactly as written in the workflow
  (fresh clone, same flags).
- [ ] **Host tests** (`make -C test/host`) pass, with new ones for new
  parsers or helpers.
- [ ] **Norwegian** for everything a user sees.

## What went wrong

### An update could be undone by saving settings

Restarting from the setup page before the new firmware had completed its
first full pass (~20 s) made the bootloader roll it back. The fix - mark
the firmware good before a setup-page restart - ran inside the `reboot`
task, whose 2 KB stack then overflowed when the marking wrote flash and
logged (found from a crash dump in 1.3.0; now 4 KB).
*Lesson:* the fix for one sequence created a crash in the same sequence.
Test the sequence again after fixing it, and size stacks for what the task
now does.

### 11.8 KB of internal RAM went unnoticed

The 1.1.0 features (route cache, update offer, log line, crash dumps, mDNS)
added 11.8 KB of static internal DRAM, and the lowest free point fell from
18 KB to 8 KB - near where WiFi starts failing. Only `/status` revealed it.
Moving ~16 KB of large static arrays to PSRAM, and binary crash dumps
(smaller interrupt stacks), left more room than before: ~90 KB free, lowest
~52 KB.
*Lesson:* measure per feature; PSRAM for big statics by default.

### Adding mDNS upgraded LVGL

Adding `espressif/mdns` made the component manager re-resolve, pulling in
LVGL 9.6, which `esp_lvgl_adapter` 0.5.3 can't patch - the build broke.
LVGL is now pinned to 9.5.0.
*Lesson:* dependency changes ripple; read the lock diff.

### A test that tested the wrong thing

An "unsigned" image passed the signature check. The same slot had just held
the identical signed image, and writing sequentially never erased the old
signature sector, so it verified the identical content. Not a way to
install foreign code, but the test proved nothing. The writer now erases
the image area plus one sector first.
*Lesson:* clean state before a negative test.

### Layout only right at the default font

At the configured 21 px text the aircraft table had no room for the route
column, so routes never showed. Night mode also paused the rotation used to
reach a screen during a test.
*Lesson:* test at the real settings.

### CI failed on GitHub but not on the Pi

The workflow cloned ESP-IDF without submodules; cJSON is one, so the host
tests couldn't compile there, while the Pi has a full ESP-IDF.
*Lesson:* run workflow steps locally as written.

### A bigger struct overflowed a task stack

Adding the daily summaries to `yr_forecast_t` (1.4.0) crashed the weather
task at start: `resample_uniform_time` copied a whole forecast into a local
variable, ~7 KB before and ~7.6 KB after, on an 8 KB stack. The bootloader
rolled back to 1.3.0 - which then applied a settings backup meant for 1.4.0
and quietly dropped the settings it didn't know. The copy now lives in
PSRAM.
*Lesson:* when a struct grows, look for it as a local variable (and as a
value copy) on task stacks; after installing, check that `/status` really
shows the new version before testing anything on it.

### Restarts hung on core 1

Since at least 1.4.0, about one restart in three (after an update, a saved
setting or a restored backup) hung inside `esp_restart()` until the RTC
watchdog reset the chip: `/status` said "vakthund i maskinvaren" instead
of "omstart fra programmet". It went unnoticed because the new firmware
still came up. The serial log showed WiFi shutting down and then nothing
until `RTCWDT_RTC_RST`; logging the core showed every hang came from a
restart task that ran on core 1. Restarting from core 0 never hung, so
`restart_device()` (components/restart.c) now hands the restart to core 0.
*Lesson:* after installing, read the restart reason in `/status`, not just
the version. A serial log is easier to get than it looks: open the port
with DTR and RTS off and keep it open, since opening it resets the board.

### Measure text with the widest letters

The route column was measured with "BGO", and "BOO" didn't fit ("O" is
wider than "G"). It is now measured with "WWW", the widest any code can be.
*Lesson:* size columns for the widest possible content, not a typical
example. The same happened again on the tide screen: its time column was
measured with "søn 88:88", and "man 02:43" wrapped ("m" and "0" are
wider). It now measures the widest weekday with the widest digit.

### A form bug that tests found

The setup page's form parser cut values before URL-decoding them, so names
with æøå (6 bytes each when encoded) were cut short and could end in a
broken escape. Found while writing the host tests; fixed by decoding first
and cutting at a whole UTF-8 character.
*Lesson:* pure code with small tests catches bugs a device test won't.

### Times couldn't be typed on a phone

The setup page's time fields (night mode, update check) asked for "TT:MM"
with `inputmode=numeric`, which on phones gives a number pad without ':'.
Tested only in a desktop browser, so nobody noticed until the user tried on
a phone. 1.6.1 also takes 2230, 930, 22.30 and 22,30 (on the page and in
the device's parser) and shows them as 22:30.
*Lesson:* check a field's keyboard (`inputmode`) against what it must
accept, and try the setup page on a phone.

### A PSRAM stack and the flash cache

In the October 2026 memory review the weather task's stack was moved to
PSRAM: ESP-IDF's code showed that, with the app running from PSRAM, flash
reads and writes leave the cache on (`SPI_FLASH_CACHE_NO_DISABLE`), and the
LVGL task already read fonts from flash on a PSRAM stack. The device then
crash-looped a few seconds after every start: `keep_firmware()` calls
`esp_ota_get_state_partition()`, which *memory-maps* the otadata partition,
and mapping still freezes the cache and asserts that the stack is internal.
The crash dump showed it at once; the weather task and the web server
(update and restore handlers) went back to internal stacks.
*Lesson:* "flash access" isn't one thing - reads and writes, mapping and
OTA calls behave differently. Before moving a task's stack to PSRAM, follow
every flash call it can make (OTA functions map flash), and flash the device
and watch `/status` for a few minutes before building on it.

### PSRAM is shared with the display

Moving work into PSRAM (LVGL's allocations, and WiFi's code by turning
off its IRAM options) freed ~100 KB of internal RAM in 1.4.3 - and made the
picture jump sideways now and then on the rain radar, which redraws every
500 ms. The RGB panel is fed from its frame buffer in PSRAM through two
small bounce buffers refilled in an interrupt; with more traffic on PSRAM a
refill came late. 1.5.1 doubled the bounce buffers to 20 lines, made the
panel resynchronise every frame, and put WiFi's code back in IRAM.
*Lesson:* internal RAM isn't the only budget - PSRAM bandwidth is, too.
After moving things to PSRAM, watch the busiest screen (the rain radar) for
a few minutes, through a download, not only `/status`.

The jumping wasn't quite gone: the bottom lines of the picture showed at
the top, and stayed there for a while. Lowering the pixel clock (1.5.3) and
moving the panel's interrupts off WiFi's core (1.5.4) didn't stop it. The
cause was the setting 1.5.1 added: with `CONFIG_LCD_RGB_RESTART_IN_VSYNC`,
ESP-IDF 5.5 never resets its count of bounce-buffer refills (the reset is
in the `#else` branch), and it picks the buffer to refill from that count.
One missed refill interrupt made it refill the buffer being sent instead of
the finished one, from then on - a lasting shift of one bounce buffer.
Without the setting the driver checks the count every frame and restarts
the panel when one is missing. 1.5.4 turns it off, and keeps the panel's
interrupts on core 1 and IRAM-safe so refills are missed less often.
*Lesson:* read the driver code behind a Kconfig option before turning it
on; its help text said "stop permanent desyncs", the code caused one. And
two guesses that "seemed to fix it" were wrong - a glitch that comes now
and then isn't fixed until it has stayed away for days.

The optional Cloudflare Tunnel service first required its token with
`${CLOUDFLARE_TUNNEL_TOKEN:?...}`. Docker Compose checks that even for a
service whose profile is off, so `docker compose up` failed for the plain
LAN setup without a tunnel.
*Lesson:* run `docker compose config` both with and without an optional
profile before calling a compose change done.

### Defaults nobody chose

Up to 1.9.1 every weather icon was decoded again for each 10-line strip it
was drawn in, and everything was compiled with `-Og`: LVGL's image cache
size and ESP-IDF's optimization level were never set, so their defaults (0
and debugging) applied. Setting both made full redraws of the weather
screens about three times faster.
*Lesson:* for anything on the drawing or fetching path, read the effective
value in `sdkconfig`, not only what `sdkconfig.defaults` sets. Measure a
change with a temporary endpoint that times `lv_refr_now` after
invalidating the screen, and remove the endpoint afterwards.

## What worked

- **Diagnostics over WiFi** (`/log`, `/status`, `/coredump`): the stack
  overflow was diagnosed from the crash dump in minutes, without the USB
  cable (opening the serial port resets the board).
- **Pure helpers split from ESP-IDF code** (`form_util`, `version_util`,
  the parsers), compiled on the Pi against stub headers with ASan/UBSan.
- **Signing instead of trusting TLS:** the update site can be plain HTTP on
  the Pi, and only our images are accepted.
- **A plan with decisions before building** (`docs/auto-update-plan.md`),
  and the backlog kept current (`docs/improvements.md`).
- **Real API replies as test fixtures** (trimmed), rather than hand-written
  ones only.

## Working practice

- Commit only when asked; the user pushes (or asks for the push).
- Publish with `scripts/publish_firmware.py`; raise `version.txt` first.
  Its signature check always prints "Signature block 1 invalid. Skipping."
  and the same for block 2: an image has room for three signatures and we
  fill only block 0 with our one key. That is harmless; what matters is
  "Signature block 0 verification successful". A wrong key fails with
  "Signature could not be verified" (exit 2), which stops the publish
  before anything is copied (checked at 1.10.2).
- Temporary test code (endpoints like `/tap` and `/crash`, fake data) is
  always removed before the final build, and the user's settings are
  restored after tests that change them (download `/config.json` first).
- Keep command output short (`head`, `grep`): large outputs used a fifth of
  a session's context.

### PSRAM headroom is shared by every screen

The image cache (1.9.2) was measured with the screens set up at the time.
Two days later the user had added satellite close-ups, departures and
tides, and the same firmware ran PSRAM out on the rain radar and
restarted - while a 500 KB allocation in the water fill had already been
failing quietly, leaving the sea white.
*Lesson:* after adding anything that holds PSRAM, walk through every
screen of the real configuration and read `psram_lavest` and
`psram_storste_blokk` in `/status`. Size large buffers to what is free
(as the rain frames now are) rather than to a fixed budget, and never let
an allocation failure quietly change what is drawn: log it, and retry.

### Touch can't be tested from the desk

The swipe menu (1.10.0) was checked by a build that opened it by itself and
a screenshot, which showed the layout but not the touch: its buttons did
nothing. LVGL passes an event up to a parent only while every object on the
way has `LV_OBJ_FLAG_EVENT_BUBBLE`; the buttons had it, the rows they sat
in didn't, so the menu's handler never heard them.
*Lesson:* attach handlers to the widgets themselves rather than relying on
bubbling, and ask the user to try anything touch-driven on the display
before calling it done.

### A new folder mounted into the update site needs a mount point

Mounting `server/montering/` into the site like `server/kalender/` stopped
the update site: the container's root is read-only and the site folder is
mounted read-only, so Docker couldn't create `/usr/share/nginx/html/montering`
and the container didn't start until `/srv/multidisplay/montering` existed.
*Lesson:* create the empty folder in `/srv/multidisplay` first, then run
`docker compose up -d update-site`, and check that `/` answers afterwards.

### Entur IDs aren't short

Flybussen's line at Einars vei is `NBU:Line:a7c15243-8873-4aee-a8b4-8d22e8ad31dc`
(45 characters), but the departure selection held IDs of up to 39: the line
was dropped as a "bad line entry" with only a warning in the log, so the
board showed the stop's other lines and nothing said FB5 was missing.
*Lesson:* size buffers for IDs from someone else's data with a margin (and a
test using the longest real one), and when something configured doesn't
show, try that exact configuration against the live API first.

### A git checkout can empty the update site's guides

The guides (`server/montering/`, `server/bruk/`) and calendar feeds are
mounted into the container from the repo as folders. A Docker bind mount
holds on to the folder itself, so when git removes and recreates it (a
rebase over them did), the container keeps the old, now empty folder: the
guides answered 403 until the container was restarted.
*Lesson:* after a pull, rebase or checkout that touches `server/`, run
`docker compose restart update-site` and check that `/montering/` and
`/bruk/` answer 200.

### A library's "prefer internal" allocations

Showing a Sonos speaker's 640-px cover (1.27.0) took the lowest free
internal RAM from ~60 KB to ~28 KB, though mbedTLS and our own buffers were
in PSRAM: esp_new_jpeg asks for its working memory - about 30 KB for a row
of blocks that wide - with `jpeg_calloc_inner`, which takes internal RAM
for as long as there is any. Found by logging
`heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` around each step of
the first poll. Its two `*_inner` allocators are now wrapped (`--wrap` in
`main/CMakeLists.txt`, `main/draw.c`) to put pieces over 4 KB in PSRAM;
decoding takes no longer.
*Lesson:* when the internal low point drops, log the minimum around each
step rather than guessing, and check a binary library's undefined symbols
(`nm`) for allocators that pick internal RAM by themselves.

### Buffers for a screen that isn't on show

The Spotify screen held two 300-px covers (360 KB of PSRAM) from start-up,
whether shown or not, which left ~370 KB free. Since 1.27.0 both the
Spotify and the Sonos screen allocate their covers when on show and give
them back on another screen (`spotify_release`, `sonos_release`), for ~650
KB free elsewhere.
*Lesson:* big buffers a screen needs only while on show belong to that
time, not to start-up.

### A reply's size against the buffer it goes into

"Recently played" asked Spotify for 50 tracks at once: about 150 KB of
indented JSON (3 KB a track, even without `available_markets`), over the
96 KB the Spotify client takes, and its cJSON tree would have been near
300 KB more - on a screen that already held the player's cover and twelve
tiles (~375 KB) with ~480 KB free. Found in review, not on the display:
the grid can only be opened by touch. It now asks 10 tracks a page (~35
KB), and the player's cover is given back while the grid is on show.
*Lesson:* size a new request's reply from a full example, indented as the
server sends it, against the buffer's limit before choosing a page size;
and add up everything one screen holds at the same moment (pictures, a
reply, its parse tree, TLS) against `psram_ledig` on that screen.

### "Oppdatert" is not "running"

An OTA of a build that changed nothing at startup answered "Oppdatert -
starter på nytt", and the display came back with the same version number
and no new errors in `/log` - but it was the previous firmware. The new
one had overflowed the `main` task's 3584-byte stack in `build_ui`
(which `app_main` runs before it returns) and the bootloader rolled
back. Only `/status` said so: `"omstart":"krasj"` and a crash dump. The
main stack is now 8 KB (freed when `app_main` returns).
*Lesson:* after an OTA, check that the new build is the one running -
something only it serves (a changed `/setup.js`, a new log line), and
`omstart` not `krasj` - before testing on it.

### A page and its script from two firmwares

A user calibrating the temperature was told "Brikken har ikke målt
temperaturen ennå" right above the chip's reading. The setup page was sent
without `Cache-Control`, `/setup.js` with `no-cache`: a browser restoring
the tab kept the page from before the display updated itself (no
`data-chip` in it) and fetched the new script, which took the missing
attribute for "no reading". The page is now `no-store`, and "no reading"
is said by the page (`data-chip=''`), not by an attribute's absence.
*Lesson:* send a page built from live state `no-store`; and let a script
treat what the page doesn't say as "unknown, leave it to the server" -
the display may have updated under an open tab.

### Cover addresses cut short

Spotify's playlist covers made of four albums ("mosaic") have addresses of
~190 characters; they were kept in 160, cut, and every one answered 404, so
those tiles stayed empty. Seen only on the device, from the log.
*Lesson:* size an address for the longest real one, and never cut it:
keep it whole or not at all (`copy_url` in `spotify_parse.c`), as a cut
address fails somewhere else, later.

### A stream address that works in curl

NRK's lyd.nrk.no addresses played in curl and a browser, so the radio
presets were made with them, and only checked as far as the speaker taking
the address. Sonos took it, but answered Play with UPnP error 701: the
address sends on through a relative redirect (`Location: /icecast/...`),
which Sonos doesn't follow. The stream server's own addresses play.
*Lesson:* a preset is checked by playing it on the speaker (muted, if
need be), not by fetching it from a computer; and give a speaker the
address that answers 200 itself.

### State copied in one branch of two

The Sonos poll copies what it needs from the screen under the lock in two
places: always, and again when the room has nothing to play. The radio
page's flag was added only to the second, so with music playing the poll
never saw the page open and the logos never came. Found from a screenshot.
*Lesson:* when a task copies several fields of shared state, add a new one
everywhere the others are copied (search for one of its neighbours).

### The firmware's size is PSRAM

The radio logos (200 KB) were first built into the firmware. Free PSRAM fell
by just as much, as `CONFIG_SPIRAM_XIP_FROM_PSRAM` copies the whole image
into PSRAM at start-up, and with the music picker's tiles open it stayed
under the memory watchdog's 300 KB: a restart every minute. The watchdog's
own comment still said ~1.3 MB was free; it is ~600 KB now, the rest gone to
the firmware's growth over time.
*Lesson:* data that isn't needed all the time doesn't go into the firmware
(fetch it, as the logos now are from the update site); look at free PSRAM
(`/status`) before and after a change that adds to the image, and leave a
screen open past the watchdog's minute when it holds memory. Memory a
screen holds on purpose is told to the watchdog (`wd_psram_held`).

### Memory held for a picture that never came

The music pickers allocated a tile's 21 KB pixel buffer before fetching its
picture, and kept it when the fetch failed: with the update site down, the
radio page's 12 tiles would hold ~260 KB for drawing their names - memory
the watchdog counts as free, as `wd_psram_held` told it. Three 10 KB static
arrays only used to stage the tiles for a moment, and a 320-byte address
copy per tile, cost another 48 KB of PSRAM for good. Found by a review, and
measured with `nm -S --size-sort build/multi_display.elf`.
*Lesson:* allocate a buffer once there is something to put in it, and free
it on every failure path; a static array in PSRAM is PSRAM gone for good, so
a buffer used for a moment is allocated for that moment. Check the biggest
`b` symbols with `nm` after adding statics.

### A buffer that doubles needs three times the body

A song's cover over AirPlay (180 KB, from the speaker's `/getaa`) wasn't
shown: the response buffer grew by doubling, to 256 KB, and the step from
128 KB to 256 KB needs both at once - 384 KB, more than was free with the
Sonos screen's other memory. The cover then stayed empty for the whole
track, as a failed cover was never asked for again. It showed after a
restart, with more memory free, which pointed at memory rather than the
picture.
*Lesson:* collect a body of known length into a buffer of that length
(`http_get_buf` takes Content-Length); compare `/status`'s lowest free
PSRAM before and after one fetch to see what it costs; let a failure that
may be passing be tried again later.

### Opening the USB serial port restarts the display

Opening `/dev/ttyACM0` to read the log restarted the display (reason "USB"),
even with DTR and RTS set low first: the ESP32-S3's USB serial resets the
chip when a port is opened.
*Lesson:* don't open the serial port of a display that is in use just to
read its log; use `/status`, screenshots, and the speaker's or service's
own replies, or say first that it will restart.


### Memory kept for a page no longer shown

The Sonos player gave back the radio page's tiles when the picker closed,
but kept the music page's, to show them at once next time. With a page of
12 playlists (~260 KB of tiles) closed, the player then fetched its cover
on top, and the lowest free PSRAM fell to 124 KB, against ~280 KB with
the tiles given back. Found by stepping through the screens one at a time
and reading `/status`'s lowest free PSRAM after each.
*Lesson:* memory held for something on show is given back as soon as it
isn't, even if that costs a refetch later; and when a lowest-free figure
drops, step through the screens and read it after each step to find the
one that does it.

### Test taps from the web don't hold off the rotation

Testing the Sonos pickers with taps sent over HTTP, the automatic rotation
moved on to a weather screen halfway, and the taps after it went to other
screens (doing nothing). A real touch holds off the rotation; a tap made by
test code doesn't, as it bypasses the touch callback. *Lesson:* send
`vis=N` to /nav (which counts as a tap) before each step of such a test,
and check a screenshot before trusting a step's result.

### A mark set only after the slow part of a poll

The ring on what plays was set at the end of the Sonos poll, after the
covers it fetches; when the list was fetched again (every 2 minutes), the
tiles were rebuilt without it and the ring stayed off for as long as the
covers took - in one test longer than the 18 s before the screenshot.
*Lesson:* state derived from other state (the ring, from what plays) is set
again wherever its tiles are rebuilt, not only at one place in the poll.

### Free memory in pieces too small

The Spotify cover stayed empty while 527 KB of PSRAM was free: the screen
allocated two 180 KB buffers for it (the cover shown, and one to decode the
next into), and the largest free block was 213 KB - the pickers' 21 KB
tiles, allocated and freed in turn, had cut the heap up. Found from
`/status`'s `psram_storste_blokk`, not the free total.
*Lesson:* look at the largest free block as well as the total; a large
buffer is allocated once and kept (decoded into in place, as the Sonos
cover is), not two of them where one does.

### A rasteriser that leaves rows out

The world map for the ISS screen (`scripts/build_world.py`) first came out
with thin sea-coloured lines straight across Russia, Asia and Canada: PIL's
polygon fill left whole rows out of the largest polygons, and neither
drawing the edges too nor nudging the corners off the pixel grid helped. A
dozen lines of scanline fill in numpy (crossings per row, half-open at the
corners) drew it right. Seen only because the result was looked at as a
picture before it went into the firmware.
*Lesson:* look at generated data as a picture, and check a suspect row by
its numbers; when a library's output is wrong in a way that resists
workarounds, a small fill of one's own beats more workarounds.

### A screen's buffer and the memory watchdog

The ISS map's 160 KB took free PSRAM on that screen to 311 KB, just over the
watchdog's 300 KB restart limit. It is given back when another screen
shows, so it is reported with `wd_psram_held`, as the music tiles are.
*Lesson:* a new screen's buffer that is held while it shows is checked
against `WD_LOW_PSRAM` on the device, and counted as held if it is given
back.

### A source that is fast from here and slow from the display

The ISS screen said it couldn't fetch the orbit: Celestrak, which answered
in 0.7 s from the development machine, sometimes took 10-20 s, past the
display's 15 s timeout (`ESP_ERR_HTTP_EAGAIN`: no reply in time). Timing a
few requests in a row, at different times, showed it.
*Lesson:* time a new source several times, not once, and give a small
download from a free service a generous timeout, a second source where one
exists, and a copy kept over restarts.
