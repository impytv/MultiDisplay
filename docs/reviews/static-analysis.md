# Static analysis (November 2026, 1.36.1)

The four findings below were fixed in 1.36.2 (docs/improvements.md,
161-164). To be run again: item 165.

The display's own C code (the 65 files of `main/` and `components/` in
`build/compile_commands.json`), the setup page's scripts and the server and
build scripts, run through the open-source analysers most often named for C:

| Tool | Version | What it is | Run on |
|---|---|---|---|
| clang-tidy with the Clang Static Analyzer | LLVM 23.1.4 (pip) | path-sensitive analysis (`clang-analyzer-*`), plus `bugprone-*` and some `cert-*` checks | the 65 C files, as Xtensa, with the build's own flags |
| cppcheck | 2.17.1 (Debian) | its own analysis of values and branches, few false alarms | the 65 C files, `--check-level=exhaustive`, warning/portability/performance/style |
| flawfinder | 2.0.20 (pip) | lists risky C calls (format strings, buffers) | `main/`, `components/`, level 3 and up |
| semgrep | 1.184.0 (pip) | rules of patterns (`p/c`, `p/default`, 446 rules) | C, JS and Python: 142 files |
| gcc `-fanalyzer` | 14.2 | (run earlier, see review-second-pass.md) | what the host tests build |

None of them was installed; they were fetched into the session's scratch
folder (pip venv, and the cppcheck .deb unpacked) and nothing was installed
on the Pi. clang ran with `--target=xtensa -nostdlibinc` and newlib's headers
from the ESP-IDF toolchain; every file parsed without errors.

## What they reported

- **clang-tidy:** 798 warnings. 680 are `bugprone-signed-bitwise` (LVGL's
  and ESP-IDF's flag macros) and 17 are enums initialized to 0 in ESP-IDF
  structs. 63 are `atoi`/`atof`/`sscanf` without error reporting, where the
  results are range-checked after or come from the device's own form. 19 came
  from the Clang Static Analyzer itself (null or garbage values, array bounds,
  pointer subtraction, `va_end`); each was followed back through the code,
  and all but one are false alarms: the analyser doesn't know that
  `cJSON_IsString(NULL)` is false, that `via_count <= ENTUR_MAX_VIA`, that
  a room is always picked when there are rooms, or that `at[]` is filled for
  every aircraft. The one left is finding 3 below. The rest were a dead store
  and integer divisions that were meant.
- **cppcheck:** no warnings. 37 style notes (pointers that could be `const`,
  two struct members never read, two conditions it misread in
  entur_client.c) and one file it couldn't parse (an `asm` label in
  wifi_provision.c).
- **flawfinder:** 30 hits at level 4: format strings that aren't literals,
  each a choice between literals or a literal macro joined to one; one
  `sprintf` into a buffer sized for exactly what it writes; "access" in the
  word access_token. None reach the code from outside.
- **semgrep:** 12 findings, none in the C code: Python scripts that call
  urllib with a URL from a variable (the build scripts and the two
  services; the radio proxy's picture addresses are allowlisted since
  1.36.1), and SHA-1 used for the radio proxy's picture ids (not a security
  use).

## Findings that hold

### 1. A long Spotify Retry-After restarts the display (medium)

Not reported as such by any tool, but found following clang's
`atoi` warning on the Retry-After header. When Spotify answers 429,
`spotify_poll` returns `Retry-After * 1000` ms, and the weather task waits
that long without a beat. The watchdog restarts it after 20 minutes
(`WD_WEATHER_MAX_S`). Spotify is known to send Retry-After of hours to a
day to an app that has asked too often, so with the Spotify screen on show
the display would restart every 20 minutes, asking again (and being refused
again) after each start. Past 4.29 million seconds the `uint32_t`
multiplication would also wrap to a short wait.
*Fix:* keep the deadline in spotify_client (no request before it, across
polls), and return at most a few minutes from `spotify_poll`, so the task
comes round and beats while it waits.

### 2. The ship service's token lifetime isn't checked (low)

ais_client.c reads `expires_in` with `(int)to_double(...)` and multiplies
it by 1000 in `pdMS_TO_TICKS`: a value over about 2.1 million (or
NaN) is undefined behaviour and gives a nonsense expiry. The check after it
for a token too long for the request (`vlen + 64 > AIS_TOKEN_MAX + 512`) can
never be true, since `vlen < AIS_TOKEN_MAX` already.
*Fix:* clamp `expires_in` to 60 s - 24 h; remove the dead check.

### 3. `app_config_from_json` writes `*n_events` without checking it (latent)

clang: with `events` given and `n_events` NULL, line 380 writes through
NULL. No caller does that today (the restore passes both, the tests
neither).
*Fix:* check `n_events` too.

### 4. Tidying (cosmetic)

The dead store in `show_login_status` (spotify.c), and `raw_ac_t`'s
`lat`/`lon`, read but never used (adsb_client.c: only their presence is).

## Running them again

Nothing needs root. In a scratch folder `$T`, with the firmware built once
(`idf.py build` makes `build/compile_commands.json`):

```bash
python3 -m venv $T/venv && $T/venv/bin/pip install clang-tidy flawfinder semgrep
cd $T && apt-get download cppcheck libtinyxml2-11 && for f in *.deb; do dpkg -x $f root; done
cp root/usr/bin/cppcheck root/usr/lib/aarch64-linux-gnu/cppcheck/   # beside its cfg/
```

clang needs a compile database of only `main/` and `components/`, with the
Xtensa-only gcc flags and the `@build/toolchain/cflags` response file taken
out, and `--target=xtensa -nostdlibinc -isystem <toolchain>/xtensa-esp-elf/include`
added (the toolchain is under `~/.espressif/tools/xtensa-esp-elf/`). Then:

```bash
C='-*,clang-analyzer-*,bugprone-*,cert-err34-c,cert-flp30-c,cert-str34-c'
C="$C,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-bugprone-signed-bitwise"
C="$C,-bugprone-easily-swappable-parameters,-bugprone-narrowing-conversions,-bugprone-reserved-identifier"
$T/venv/bin/clang-tidy -p $T/db --checks="$C" FILE.c      # each of the 65, e.g. with xargs -P4
LD_LIBRARY_PATH=$T/root/usr/lib/aarch64-linux-gnu $T/root/usr/lib/aarch64-linux-gnu/cppcheck/cppcheck \
  --project=$T/db/compile_commands.json -j4 --check-level=exhaustive \
  --enable=warning,portability,performance,style --suppress=missingIncludeSystem \
  --suppress='*:*/esp-idf/*' --suppress='*:*/managed_components/*' --suppress='*:*/build/*'
$T/venv/bin/flawfinder --minlevel=3 --columns main components
$T/venv/bin/semgrep scan --config p/c --config p/default --metrics=off main components server scripts
```

The whole run takes a few minutes on the Pi (clang-tidy about one, with four
at once). Expect the noise described above; what is new since this report
is what to look at.
