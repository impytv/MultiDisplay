# Lessons learned

From the work on firmware 1.0.0–1.3.0 (September 2026): automatic updates,
diagnostics, the Norwegian setup page, host tests and the air screen. What
went wrong, why, and what to do next time.

## Checks before calling a change done

- [ ] **Internal RAM:** compare `idf.py size` (the DIRAM line) with the
  previous release, and look at `/status` (free and lowest internal DRAM)
  after a few minutes on the device. Large static arrays get
  `EXT_RAM_BSS_ATTR` (PSRAM) from the start.
- [ ] **Dependencies:** read the `dependencies.lock` diff after adding or
  changing a component, and pin anything the build is sensitive to.
- [ ] **Sequences, not just features:** test what people do in a row -
  above all "install an update, then save or restore settings within 20 s",
  and a power cut or restart in the middle of things.
- [ ] **Real settings:** test with the display's actual settings (21 px text,
  five locations, night mode on) as well as the defaults.
- [ ] **Stacks:** when adding work to a task, check its stack size; anything
  that writes flash or logs needs more than 2 KB. When a struct grows,
  look for it as a local on a task stack.
- [ ] **The new version is running:** after installing, check the version
  in `/status` (and that there's no crash dump) before testing on it.
- [ ] **Widest content:** size text columns for the widest possible value
  ("WWW", "-88°"), not a typical one.
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

### Measure text with the widest letters

The route column was measured with "BGO", and "BOO" didn't fit ("O" is
wider than "G"). It is now measured with "WWW", the widest any code can be.
*Lesson:* size columns for the widest possible content, not a typical
example.

### A form bug that tests found

The setup page's form parser cut values before URL-decoding them, so names
with æøå (6 bytes each when encoded) were cut short and could end in a
broken escape. Found while writing the host tests; fixed by decoding first
and cutting at a whole UTF-8 character.
*Lesson:* pure code with small tests catches bugs a device test won't.

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
