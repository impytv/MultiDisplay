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
- Temporary test code (endpoints like `/tap` and `/crash`, fake data) is
  always removed before the final build, and the user's settings are
  restored after tests that change them (download `/config.json` first).
- Keep command output short (`head`, `grep`): large outputs used a fifth of
  a session's context.
