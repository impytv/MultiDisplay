# Review: a daily user and long-term maintainer (2026-11-04, 1.35.0)

The angle here is someone who has the display on the wall every day and
will keep the code going for years. The questions were: does what the
screen says match what is true (time, date, age of the data, units), does
it read as one Norwegian product across screens, and where has logic been
copied and then drifted. Everything below came from reading the code. Each
finding has a concrete scenario, and none of them is already in
`docs/improvements.md` or `docs/lessons-learned.md`.

## Visible to the user

### High

**1. The week screen shows yesterday as its first card after midnight, and never says its forecast is old**
`main/week.c:124-136`, `main/week.c:207-215`, `components/yr_client.c:139-155`,
`components/http_util.c:252-253`

- **What is wrong:** `parse_days` groups the forecast by local day, starting from
  the first timeseries entry. `week_draw_cb` draws `days[0..6]` without skipping
  days that are over. The forecast is only parsed again when MET sends a new one.
  Until MET's `Expires` time, `http_get_body_cached` answers `HTTP_NOT_MODIFIED`
  without asking at all, so the old `days[]` stay. Unlike the weather screen
  (`weather.c:1112`, `WX_STALE_S`), the "Oppdatert kl." label on the week screen
  never turns into an orange "Sist oppdatert" label.
- **Scenario:** The forecast is fetched at 23:50 on Tuesday 3 November. Its first
  entry is 23:00 local, so `days[0]` is Tuesday with one hour in it. From 00:00
  to about 00:20 on Wednesday (until `Expires`), the first card reads
  "tirsdag 3.11." with one hour's temperature for both the high and the low. The
  "I dag" card is the second one, and only six days of the coming week are left.
  If the WiFi is down for a weekend, the screen shows Friday's cards on Monday,
  with "Oppdatert kl. 14:02" in the normal colour.
  - A related point: in the evening the "I dag" card shows only the hours that
    are left. At 22:00 it can read 3°/2° on a day whose high was 9°.
- **Fix:** In `week_draw_cb`, skip days whose `start` is before today's local
  midnight. Give the label the same `WX_STALE_S` orange "Sist oppdatert" the
  weather screen uses.
- **Confidence:** confirmed

**2. Wrong local time for the nightly restart around a DST change: an extra restart in the October night**
`main/main.c:1103-1115`

- **What is wrong:** `compute_next_nightly_reboot` finds today's 02:00 using
  `tm_isdst` copied from *now*. If that time has passed, it adds `24 * 3600`
  seconds instead of moving to the next calendar day. The device restarts at
  02:00 every night, so this "+24 h" path runs on every boot. On the night the
  clocks change, the restart lands an hour off.
- **Scenario:** The device boots on Sunday 2026-10-25 at 02:00:15 CEST. Today's
  02:00 counts as passed, and +24 h gives Monday 00:00 UTC, which is 01:00 CET.
  It restarts at 01:00 on Monday, works out 02:00 CET again, and restarts again
  at 02:00. That is two restarts in one night, and the backlight comes on at each
  boot (`main.c:1555`) even with "slå av skjermen om natten". If newlib resolves
  the ambiguous 02:00 the other way, the extra restart is at the second 02:00 on
  Sunday instead.
  - In March the 02:00 restart becomes 03:00 CEST. That is harmless, but it is
    an hour late.
- **Fix:** Use `local.tm_mday += 1; local.tm_isdst = -1;` and `mktime` again,
  instead of `+= 24 * 3600`. Set `tm_isdst = -1` for today's target too. This is
  a pure function that is easy to test: move it next to `update_schedule.h` and
  add host tests for the last Sundays of March and October.
- **Confidence:** confirmed. The +24 h error is certain; which 02:00 is picked
  depends on newlib's `mktime`, but either way there is an extra restart.

### Medium

**3. The month calendar keeps a failing calendar's events for ever, with no hint**
`main/month.c:727-737`, `main/month.c:767`

- **What is wrong:** When one feed fails, `fetch_events` copies that feed's old
  events into the new list if `stamp_fresh(&s_cal_at, MON_CAL_KEEP_MS)`. But
  `s_cal_at` is stamped on every partly successful fetch (line 767), so the
  check is always true. The calendar screen does this correctly: it times each
  feed separately (`calendar.c:349-352`, `s_feed_at[i]`), drops a feed's events
  after 12 h, and says "(kalender 2 ikke oppdatert)" in orange. The month screen
  has no "updated" label and no hint at all.
- **Scenario:** Feed 2's secret address is reset in Google Calendar on Monday.
  On the calendar screen its appointments disappear after 12 h. On the month
  screen the same appointments (including ones that were moved or cancelled
  since) stay for weeks, refreshed every 15 minutes from the copy.
- **Fix:** Keep a `fetch_stamp_t` per feed in `month.c`, as `calendar.c` does.
  Better still, give both screens one shared "merge a partial fetch" helper.
  Show a small orange note when a feed failed.
- **Confidence:** confirmed

**4. The overview never tells you a forecast is old**
`main/weather.c:1467-1545`

- **What is wrong:** `update_overview` uses `s_fc_cache[i]` whatever its age.
  Its only limit is `OV_MAX_OFF_S` (3 h) between a column and the nearest
  forecast point, and a 9-day forecast always has a point near now. `s_fc_when`
  is never read on the overview. The weather screen turns orange after 30
  minutes, but the overview, which is often the screen left up, looks current.
- **Scenario:** MET fetches fail from Saturday afternoon. On Monday morning the
  overview shows Saturday's forecast for "kl 08" with no mark. Only the
  "Ingen internett" badge hints at it, and only while the network is down, not
  when MET alone fails.
- **Fix:** Grey a row out, or add an orange "Sist oppdatert kl. HH:MM" under the
  title, when any row's `s_fc_when` is older than `WX_STALE_S`. One shared
  "stale label" helper would also cover findings 1 and 9.
- **Confidence:** confirmed

**5. Decimals: a comma on some screens, a point on others**
`main/weather.c:615`, `main/weather.c:813`, `main/weather.c:1540`,
`main/radar.c:246`, `main/radar.c:760`, `main/radar.c:1019`, `main/radar.c:1164`,
compared with `main/week.c:76-80` and `main/clockscreen.c:156`

- **What is wrong:** Bokmål uses a decimal comma. The week screen goes out of
  its way to write "2,1 mm" and the clock writes "6,5°". The weather chart writes
  "3.4°" and "0.3–1.2 mm", the overview "1.2 mm", the radar "10.7 km" and
  "12.5" on its rings, and the ship and aircraft tables "4.5". This is the same
  kind of number in two styles.
- **Scenario:** The weather screen's marker says "0.4–2.1 mm". One tap later,
  the week screen says "0,4–2,1 mm".
- **Fix:** One `fmt_dec(buf, n, v, decimals)` in `draw.c` that writes a comma.
  Use it on every screen (radar, weather, overview, ISS) and drop the
  hand-rolled `strchr(out, '.')` in `week.c`.
- **Confidence:** confirmed

**6. "-0°" and "+0 cm" on the screen**
`main/weather.c:1532-1538`, `main/weather.c:615`, `main/tide.c:172`

- **What is wrong:** `%.0f` of a value between -0.5 and 0 prints "-0".
  - The overview's cells use `%.0f` with blue for `< 0`.
  - The tide's "Vær og vind: %+.0f cm" prints "+0" or "-0" for any value under
    0.5 cm.
  - The weather chart's markers use `%.1f` on *interpolated* values
    (`resample_uniform_time`), so -0.04 shows as "-0.0°".
  - `%.0f` rounds halves to even, so 2.5 shows as "2" on the overview but "3°"
    on the week screen (`lroundf`).
  - The colour rules differ for exactly 0: the week screen's `> 0` counts it as
    cold, the weather screen's `< 0` as warm.
- **Scenario:** At 07:00 the temperature is -0.3 °C. The overview shows a blue
  "-0°" while the week screen shows "0°". On a calm day the tide screen shows
  "Vær og vind: +0 cm".
- **Fix:** Round once with `lroundf` and print the integer (with an explicit
  sign only when it isn't 0). One shared `fmt_temp` would also settle the
  colour rule for 0.
- **Confidence:** confirmed

**7. The rain marker says "1.2–1.2 mm" when it is raining now**
`main/weather.c:813-814`, `main/weather.c:1569-1571`

- **What is wrong:** `merge_nowcast` sets `precipitation_min_mm =
  precipitation_max_mm = rate` for the nowcast steps. `place_precip_markers`
  always prints "%.1f–%.1f mm". The week screen avoids the same problem with
  `mm_differ` (`week.c:84`).
- **Scenario:** Rain is falling now at 1.2 mm/h, the strongest in the next 48 h,
  so its peak is in the nowcast part. The marker reads "1.2–1.2 mm".
- **Fix:** When min and max round to the same tenth, print the single value
  ("1,2 mm"), as `week.c` does.
- **Confidence:** confirmed

**8. A timed event over several days says "til 14:00" today when it ends days later**
`main/calendar.c:120-133`, `main/calendar.c:211-215`

- **What is wrong:** `on_day` puts a timed event that started before today
  under "I dag". It is then labelled "til HH:MM" from `ev->end`, whatever day
  that is. Timed events never show on later days.
- **Scenario:** "Konferanse" runs Monday 09:00 to Wednesday 14:00. On Tuesday,
  "I dag" shows "til 14:00 Konferanse", which reads as ending today at 14:00.
  Wednesday's heading doesn't list it at all.
- **Fix:** If the end is on a later day, say "til ons 14:00" (or
  "til i morgen 14:00"). Also show the event under each day it covers, as
  all-day events already are.
- **Confidence:** confirmed

### Low

**9. Old data looks different on every screen**
`main/departures.c:557-559`, `main/radar.c:931-936`, `main/weather.c:1113`,
`main/air.c:199`

- **What is wrong:** The screens disagree on how they show that data is old:
  - Departures replaces the time with "Kunne ikke oppdatere - viser siste data",
    so it no longer says *when* the data is from.
  - The rain radar keeps playing the old hour with "Radarbilde kl. HH:MM" in the
    normal colour.
  - Weather, air, tide and calendar say "Sist oppdatert kl. HH:MM". For weather
    that can be days old (forecasts never expire), and no day is given.
- **Scenario:** After a weekend without internet, the weather screen says
  "Sist oppdatert kl. 14:02" on Monday, which reads as today.
- **Fix:** Make one helper used everywhere: "Sist oppdatert kl. 14:02" in
  orange, with "i går" or the weekday added when the time is not today.
- **Confidence:** confirmed

**10. Today's headings switch at the next poll, not at midnight**
`main/calendar.c:325-395` (poll every 5 min, no timer), `main/week.c:224-231`
(the 1 s timer only moves the clock)

- **What is wrong:** `month.c:624` redraws when the day changes. The calendar
  and week screens are only drawn again when they poll.
- **Scenario:** At 00:00-00:05 the calendar's "I dag" still lists yesterday's
  all-day events, and "I morgen" is today. The week screen's "I dag" highlight
  stays on yesterday for up to 10 minutes.
- **Fix:** In the screens' existing 1 s timers, invalidate the canvas when the
  local day key changes, as `month.c` does.
- **Confidence:** confirmed

**11. The week screen's wind line has no fallback when it doesn't fit**
`main/week.c:178-186`

- **What is wrong:** The rain line just above it is tried again with whole
  numbers when it is wider than the 105 px card (`w - 8`). The wind line,
  "14 (22) m/s", is drawn into the same width with no check. `draw_text` gives
  one line of height, so text that is too wide wraps and loses its end (see
  lessons-learned, "A label cut with ... that wrapped instead").
- **Scenario:** With the body text at 20-21 px (the setup page allows up to 21)
  on a day with gusts of 10 m/s or more, "m/s" goes missing.
- **Fix:** Measure with `draw_text_w`. If it is too wide, drop " m/s" or the
  gust in parentheses.
- **Confidence:** plausible (depends on the font's widths)

**12. Small differences in Norwegian between screens**
`main/weather.c:1493`, `main/tide.c:85-100` compared with `main/iss.c:420-436`

- **What is wrong:**
  - The overview's column headers are "kl 14". Everywhere else it is "kl."
  - The +6/+12/+18 h columns don't say they are tomorrow ("kl 02").
  - The tide screen lists tomorrow's tide as "tor 05:36", while the ISS screen
    says "i morgen 05:51" for the same kind of time.
- **Scenario:** At 20:00 the overview reads "kl 20, kl 02, kl 08, kl 14", with
  no sign that three of the columns are tomorrow.
- **Fix:** Use "kl." everywhere, and mark the first column of the next day
  ("i morgen kl. 02" or a divider). Share one day-relative formatter (see
  finding 14).
- **Confidence:** confirmed

## Maintainability

### Medium

**13. The setup page's time parser has no host test, and has bitten before**
`components/wifi_provision.c:1300-1318`, `components/setup_page.js` (`hm()`),
`components/app_config_json.c:198-207`

- **What is wrong:** `parse_hhmm` is pure and `static` in the 2,300-line
  `wifi_provision.c`, so `test_form.c` can't reach it. Its rules (2230, 930,
  22.30, 22,30) were added after "Times couldn't be typed on a phone" in
  lessons-learned. There are three parsers with three rule sets: the JS regex,
  `parse_hhmm`, and `get_hhmm` for backups, which takes "7:5" but not "0730".
- **Scenario:** A later change to the JS regex (say, allowing "7" for 07:00) is
  accepted by the page and silently refused by the device. The field keeps its
  old value, and no test catches it.
- **Fix:** Move `parse_hhmm` to `form_util.c` with host tests, and use it from
  `get_hhmm` too.
- **Confidence:** confirmed

**14. Date and time helpers copied five or more times, and already drifting**
`components/civil_time.h` compared with `components/own_events.c:102-124`,
`components/ical.c:62-84`, `main/iss.c:426-433`. Local midnight in
`calendar.c:97`, `month.c:694`, `events.c:93`, `own_events.c:126`,
`clockscreen.c:175`, `yr_client.c:139`, `sun.c:91` and `main.c:1103`.
Weekday and month name tables in `calendar.c`, `week.c`, `iss.c`, `tide.c`,
`clockscreen.c`, `month.c` and `events.c`.

- **What is wrong:** `civil_time.h` exists, but `own_events.c` and `ical.c` each
  carry their own Hinnant `days_from_civil`/`civil_from_days`. `iss.c` works out
  leap years inline. There are seven or more weekday tables, and the
  "today / tomorrow / weekday" formatters behave differently: tide uses
  "tor 05:36", ISS uses "i morgen 05:51", calendar uses "I morgen" headings.
  The one local-midnight helper that skipped `tm_mday += n` /
  `tm_isdst = -1` is the restart time in finding 2.
- **Scenario:** Finding 2 is the drift already happening: of eight "next local
  day" implementations, one uses +24 h and is wrong twice a year.
- **Fix:** Put `civil_from_days`, `local_midnight(t, days)`, the name tables and
  a `day_relative_text()` in one shared header or C file with host tests. Then
  remove the copies screen by screen.
- **Confidence:** confirmed

**15. Two calendar fetch loops with different rules for keeping and retrying**
`main/calendar.c:325-395` compared with `main/month.c:706-774`

- **What is wrong:** Both call `cal_fetch` for the same feeds every 15 minutes,
  but in different ways:
  - Calendar keeps a failed feed's events by that feed's own age and retries
    once after 60 s.
  - Month keeps them by the overall age (finding 3) and uses `backoff_t`.
  - Calendar shows the failed feeds; month shows nothing.
  - Two fetches of possibly several MB each run every 15 minutes when both
    screens are on.
- **Scenario:** Finding 3 is the visible result.
- **Fix:** One calendar cache in `events.c` or a small `cal_cache.c`, with
  per-feed stamps, that fetches the union of the windows both screens need.
  Both screens read from it.
- **Confidence:** confirmed

### Low

**16. `app_config_erase()` is dead code with a documented promise**
`components/app_config.c:567`, `components/app_config.h:318-323`

- **What is wrong:** Nothing calls it. The header promises "the next boot comes
  up in the setup portal", but no page, button or path can trigger that.
- **Scenario:** A maintainer looking for "factory reset" finds the function and
  assumes the feature exists.
- **Fix:** Either remove it, or wire it to a confirmed button on the setup page
  ("Tilbakestill alt").
- **Confidence:** confirmed

**17. No test that a backup round-trips the whole struct**
`test/host/test_config.c:60-151`

- **What is wrong:** The round-trip test checks the fields that existed when it
  was written, one by one. A new `app_config_t` field that is added to
  `app_config_to_json` but forgotten in `app_config_from_json` (or the other
  way round) only fails if someone also remembers to add a `CHECK`.
- **Scenario:** A 1.36 "sonos sleep timer" setting is saved in backups, but
  restoring a backup resets it. No test fails.
- **Fix:** Add a test that sets every field to a non-default value, runs
  to_json → from_json into a defaults struct, and `memcmp`s, with an explicit
  list of the fields that are deliberately not backed up (`web_pass`,
  `cal_url`, `device_name`, `temp_offset_dc`, `screen_ctl`, `nav_page`).
- **Confidence:** confirmed (the gap). There is no current bug.

**18. Pure functions with known edge cases and no host tests**
`main/departures.c:72-87` (`dep_fmt_time`), `main/week.c:64-87`
(`deg`/`mm`/`mm_differ`), `main/iss.c:421-436` (`day_time_text`, which handles
the year boundary itself), `main/tide.c:85-98` (`when_text`),
`main/weather.c:914-935` (`rain_text`)

- **What is wrong:** These are pure functions with boundaries that have bitten
  this project before: "Nå" for anything under a minute (see improvements #3),
  -0, 31 December → 1 January, and DST. They live in screen files, which aren't
  in the host-test build.
- **Fix:** As each one is moved into the shared helpers (finding 14), add a
  host test.
- **Confidence:** confirmed (the gap)

## Checked and found sound

- **Settings form and locations:** `clkout`/`issloc` follow their location
  through compacting and the ▲▼ moves (`wifi_provision.c:1592-1636`,
  `setup_page.js` CS swap). Every field on the page is parsed, and every
  `app_config_t` setting is read by some screen.
- **Backup restore:** it works on a copy, rejects text that is too long,
  re-sanitizes, and leaves out the secrets it says it leaves out.
- **Own events:** monthly events on the 29th-31st, 29 February birthdays,
  `own_prune` by calendar date, and "om N dager" rounding across DST
  (`events.c:245`).
- **Clock screen:** half degrees with a comma and no "-0", the ISO week, the
  day change for "I dag / I morgen", and the outdoor value hidden after 30 min.
- **ISS day text across the year end:** `iss.c:428-433`.
- **The update check schedule** (`update_schedule.h`), including periods before
  the epoch and the default 03:30, which exists on both DST nights.
- **Night dimming:** a window that wraps past midnight, an empty window when
  start equals end, and running once a second.
- **Alerts:** expired alerts are hidden even when fetches fail
  (`weather.c:1325`).
- **The tide sign:** "Vær og vind" is forecast minus tide alone, the right way
  round.
- **Screen names:** the display menu and `nav_page.js` agree, and all
  user-facing text is Norwegian with 24-hour times throughout.
