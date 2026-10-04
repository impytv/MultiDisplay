# Automatic firmware updates – implementation plan

**Status: implemented** (firmware 1.0.0). The notes under "As built" at the
end record where the code differs from the plan and what was tested.

The display checks a configurable URL for a newer firmware and installs it by
itself. A small static website hosts the firmware and a manifest that
describes the latest version. The device refuses any image that is not ours,
is not meant for this board, or is not newer than the one it runs.

## Why the protection has to be in the image, not the connection

TLS certificate verification is off for the whole firmware
(`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`, see `sdkconfig.defaults`), because
the big RSA chains don't fit in internal RAM. So HTTPS alone does not prove
where a download came from. The firmware image must carry its own proof:
a **signature** that the device checks before it will boot the new image.
With that in place the hosting can be plain HTTP on any web server, and a
hijacked server or DNS can at worst offer an old or no update – never code
of its own.

## Protection against the wrong firmware

Each check runs before the next step. Any failure aborts the update and
leaves the running firmware untouched.

| Check | Stops | Where |
|---|---|---|
| Manifest `project` and `board` match this firmware | Firmware for another project or board | Manifest, before downloading |
| Manifest `version` is newer than the running one | Downgrades and endless reinstalls | Manifest, before downloading |
| Manifest `version` isn't the one that was just rolled back | Retrying a broken release every night | Manifest, before downloading |
| `esp_app_desc_t` in the first chunk: `project_name` and `version` match the manifest | A manifest that points to the wrong file | First 4 KB of the download |
| SHA-256 of the download equals the manifest's `sha256`; size equals `size` | Truncated or corrupted downloads | After the last chunk |
| **RSA-3072 signature (Secure Boot v2 scheme)** checked by `esp_ota_end()` | Anything not built and signed by us | ESP-IDF, before the boot partition is switched |
| App rollback (already on): the new firmware must complete one full fetch pass (`keep_firmware()`) | Signed but broken firmware | Bootloader, on the next restart |

### Signing without burning eFuses

ESP-IDF supports signed updates **without enabling Secure Boot**:

```
CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
# CONFIG_SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT is not set
CONFIG_SECURE_BOOT_V2_RSA_ENABLED=y
CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y
CONFIG_SECURE_BOOT_SIGNING_KEY="keys/ota_signing_key.pem"
```

- No eFuses are burnt and the bootloader stays the same. The board can always
  be re-flashed over USB.
- The public key comes from the signature block of the **running** firmware.
  A new image is accepted only when it is signed with the same key.
- This also covers the manual `POST /ota` upload: it will accept signed
  images only.
- The key is generated once with
  `espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys/ota_signing_key.pem`.
  - It stays **local on the Pi, never in git or on GitHub**.
  - `keys/` goes in `.gitignore`.
  - The publish script refuses to run if anything under `keys/` is tracked
    by git.
  - It has no passphrase.
  - If the key is lost, the next update needs a USB flash.
- The signature check (RSA-3072) runs in mbedTLS, which is already in PSRAM,
  so internal RAM is not an issue.
- **Migration:** today's firmware doesn't check signatures. The first signed
  build is installed through the existing `/ota` upload, which only checks
  the project name. From then on only signed images are accepted.

The manifest itself is not signed. It can only point to an image that
passes all the checks above, so signing it would add little.

## Versions

- `git describe` currently gives a hash that can't be ordered. Instead, add a
  `version.txt` holding `MAJOR.MINOR.PATCH` (starting at `1.0.0`). CMake
  reads it into `PROJECT_VER`, which ends up in `esp_app_desc_t.version`.
- The device compares versions numerically, part by part, so `1.10.0` is
  newer than `1.9.3`.
- The release script refuses to publish a version that isn't higher than the
  one already on the site.

## Manifest

`manifest.json` lives at the URL set on the device, here
`http://192.168.0.119:8070/manifest.json`:

```json
{
  "project": "multi_display",
  "board": "esp32s3-lcd-4.3",
  "version": "1.3.0",
  "url": "firmware/multi_display-1.3.0.bin",
  "size": 1712384,
  "sha256": "9f2c…",
  "released": "2026-10-02",
  "notes": "Direction for train lines; rain animation waits for all frames."
}
```

- `url` may be relative to the manifest's own URL.
- The device rejects a manifest over 4 KB.
- `board` is a new constant (a Kconfig string, default `esp32s3-lcd-4.3`).
  It separates future hardware variants that share the project name.

## Device side

### Configuration (`components/app_config`)

| Field | NVS key | Default |
|---|---|---|
| `ota_auto` – check and install automatically | `otaauto` | **off** |
| `ota_url` – manifest URL (max 200 chars) | `otaurl` | Kconfig `OTA_DEFAULT_URL`, `http://192.168.0.119:8070/manifest.json` |

Automatic install is **off by default**, including after this firmware is
installed on a display that has no `otaauto` key yet. It must be ticked on
the setup page. "Check now" and "Install now" work whether it is ticked or not.

### Setup page (`components/wifi_provision.c`)

A new "Automatic updates" fieldset next to the existing firmware upload:

- A checkbox, "Install new firmware automatically".
- The manifest URL.
- A read-only status line:
  - "Running 1.2.0. Last check 03:30: up to date"
  - or "…: 1.3.0 failed: signature not valid".
- A **Check now** button (`POST /ota/check`) that queues a check right away,
  even when automatic install is off.
- When a newer version is available, the page shows it with its release
  notes and an **Install now** button (`POST /ota/install`). A person
  pressing the button installs straight away; only automatic installs wait
  for the night.

Available updates are shown **only on the setup page**, never on the display
itself.

The manual firmware upload (`POST /ota`) still accepts an **older** version,
provided it is signed with our key. This way a bad release can be undone by
hand. Only the automatic updater requires a newer version.

### Update module (`main/updater.c/.h`)

- Move the image writing out of `h_ota` into a shared writer:
  - `ota_writer_begin()` checks the header and `esp_app_desc_t`.
  - `ota_writer_write()` writes each chunk.
  - `ota_writer_finish()` runs `esp_ota_end()` and sets the boot partition.
  - Both the manual upload and the updater use it, so the checks live in one
    place.
- `updater_check(bool install)`:
  1. Fetches the manifest with `http_get_body()` (max 4 KB) and parses it
     with cJSON.
  2. Runs the manifest checks from the table above. The rolled-back version
     is read with `esp_ota_get_last_invalid_partition()` and
     `esp_ota_get_partition_description()`, so no extra state is stored.
  3. Streams the image with `esp_http_client` in 4 KB chunks (in PSRAM)
     straight into the writer, hashing as it goes. Nothing is held in
     memory beyond one chunk.
  4. Compares the SHA-256 and size, then calls `esp_ota_end()` for the
     signature check, sets the boot partition and restarts.
- **Scheduling:** runs in the weather task between polls, so it never
  overlaps a forecast or radar fetch.
  - A first check 10 minutes after boot. It only records what is available
    for the setup page and never installs.
  - Then the scheduled checks: at a local time and every so many hours
    from it, both set on the setup page (October 2026; before, one fixed
    check a day at 03:30). The default stays 03:30 every 24 hours, while
    the screen is dimmed and after the 02:00 nightly restart, so memory is
    fresh. **These are the only checks that install automatically.** A
    version found in between waits for the next one. A scheduled check
    missed by a restart is still made up to 90 minutes (at most half the
    interval) after its time; the schedule is counted from the time of day,
    so the nightly restart doesn't reset it.
  - When a check fails, the next one is in 1 hour. Every later failure
    doubles the wait, up to 24 hours.
  - "Check now" and "Install now" wake the task.
- **On screen during an install:** the status label shows "Oppdaterer programvare 1.3.0 … 45 %"
  while downloading, and the watchdog heartbeat is kept going. A download
  is about 1.7 MB, roughly 15–30 s.
- **Safety:** the watchdog's memory check could fire during the download.
  If it does, the half-written partition is simply never selected, because
  the boot partition only changes after all checks pass.

## Website (on the Raspberry Pi)

The site is hosted on the Raspberry Pi that builds the firmware
(`raspberrypi5`, 192.168.0.119), and is reachable on the home network only.

- **Transport:** plain HTTP is fine, because the signature is what protects
  the image. The display doesn't verify TLS certificates anyway.
- **Web server:** nginx in a Docker container
  (`nginxinc/nginx-unprivileged`), from `server/compose.yaml`. It moved
  there from a host nginx in October 2026; the URL stayed the same.
  - It uses its own port, **8070**, so port 80 stays free for anything
    else.
  - It serves `/srv/multidisplay`, mounted read-only, with directory listing
    off. The folder is mounted rather than single files, because the publish
    script replaces files by renaming.
  - `server/nginx.conf` is checked in and mounted into the container.
  - The container runs as a non-root user with a read-only filesystem and
    no capabilities, and `restart: unless-stopped` brings it back after a
    reboot.
- **Address:** the display uses the IP address, not `raspberrypi5.local`.
  ESP-IDF's HTTP client does not resolve mDNS names without extra
  components. This means the Pi's address must not change: give it a DHCP
  reservation in the router. The URL can always be changed on the setup page.

```
/srv/multidisplay/          (owned by the user who publishes)
  index.html            list of versions, notes, current version, manifest URL
  manifest.json         the latest release (stable)
  manifest-test.json    the latest test release
  firmware/
    multi_display-1.2.0.bin
    multi_display-1.3.0.bin
```

### Test manifest

There are two channels, each with its own manifest file. A display follows
the channel whose manifest URL is set on its setup page.

| Channel | Manifest |
|---|---|
| stable | `http://192.168.0.119:8070/manifest.json` (the default) |
| test | `http://192.168.0.119:8070/manifest-test.json` |

- A release is first published to test.
- Once it has run well, it is **promoted** to stable. This copies the test
  manifest to `manifest.json` with the same image and SHA-256, so no
  rebuild is needed.
- With a single display, publishing straight to stable is allowed too.

### Publishing

`scripts/publish_firmware.py` runs on the Pi, in this repository:

```
publish_firmware.py --channel test --notes "Direction for train lines"
publish_firmware.py --promote 1.3.0      # test -> stable
publish_firmware.py --channel stable --notes "..."
```

`--notes` is required. The release notes are written by hand for now.

The steps for a publish:

1. Checks the working tree is clean and `version.txt` is higher than the
   channel's current manifest.
2. Runs `idf.py build`, which signs the image with the key.
3. Checks the signature with `espsecure.py verify_signature`, so an unsigned
   build can never be published.
4. Copies the image to `/srv/multidisplay/firmware/`.
5. Writes `manifest.json` last, with size and SHA-256, through a temporary
   file and a rename. A display that checks mid-publish sees either the old
   manifest or the complete new one.
6. Stores the notes in `releases.json` next to the images, and regenerates
   `index.html` from that list. The list shows which version each channel
   is on.
7. Tags the commit `v1.3.0`.

The site folder is a script argument, so a test site can be used too. The
`index.html` page is plain HTML with no scripts. Old images stay available
so a specific version can be installed through the manual upload.

## Order of work

1. **Signing.** Add the signing key and sdkconfig, sign the builds, and
   install the first signed build through `/ota`. Check that an unsigned or
   wrongly signed upload is refused.
2. **Versions.** Add `version.txt`, `PROJECT_VER` and a version compare
   function.
3. **Shared writer.** Move the image writing out of `h_ota` into the writer.
4. **Updater.** Add `updater.c` with the manifest checks, streaming, SHA-256
   and scheduling.
5. **Settings.** Add the config fields, the setup page fieldset and the
   `/ota/check` endpoint.
6. **Website.** Run nginx on the Pi, add `server/nginx.conf`
   and `publish_firmware.py`.
7. **Tests on the device:**
   - A normal update.
   - Refused cases: wrong project, wrong board, older version, bad SHA-256,
     unsigned image, image signed with another key.
   - A signed build that never marks itself valid, to check that it rolls
     back and isn't retried.
   - The server going away in the middle of a download.
8. **README.** Update it: the key, how to publish, and how to recover over
   USB.

## Decided

- **Hosting:** nginx in Docker on the Raspberry Pi, plain HTTP on port 8070
  on the LAN. Optionally also reachable from outside over HTTPS through a
  Cloudflare Tunnel (the `tunnel` service in `server/compose.yaml`, turned
  on by `server/.env`; added October 2026). Nothing is opened on the router.
  The manifest's image URL is relative, so the same site works at both
  addresses.
  The display uses the Pi's IP address (192.168.0.119) for now. The URL can
  be changed on the setup page if the address changes.
- **Automatic install:** off by default.
- **Signing key:** kept locally on the Pi, never in git or on GitHub.
- **Manual downgrades:** allowed through the setup page upload, for signed
  images.
- **Install time:** automatic installs happen only at the scheduled checks
  (03:30 every 24 hours unless changed on the setup page).
  "Install now" on the setup page installs straight away.
- **Test channel:** `manifest-test.json` next to `manifest.json`, and a
  publish script that can promote a release from test to stable.
- **Release notes:** written by hand with `--notes`.
- **Available updates:** shown only on the setup page.
- **Versions:** start at `1.0.0`.

## As built

- **Code:** `components/ota_writer.c` (shared image writer),
  `main/updater.c` (manifest, download, schedule, `/ota/*` endpoints),
  `scripts/publish_firmware.py`, `server/compose.yaml`, `server/nginx.conf`,
  `version.txt`.
- **Erase first:** the writer erases the image's size plus one more sector
  before writing, instead of erasing as it goes. The extra sector is where
  an unsigned image's signature would be looked for, and a signature left
  there by an earlier image in the same slot would otherwise verify an
  identical unsigned copy.
- **SHA-256 in the writer:** `ota_writer_finish()` compares the SHA-256
  before `esp_ota_end()`, so nothing is selected on a mismatch.
- **Restart from the setup page keeps the firmware:** a save or upload
  restarts the display. If that happened before `keep_firmware()` had run
  on a freshly updated firmware, the bootloader went back to the old one.
  A restart asked for on the setup page now marks the running firmware as
  good first.
- **Trial publishing:** `--site` points the script at another folder, and
  `--allow-dirty` publishes from uncommitted changes (not tagged).
- **Tested on the display** (2026-09-28, trial site on port 8070):
  - Normal update through "Install now", 2.3 MB in about 28 s.
  - Automatic install in the window, with the window moved for the test.
  - Refused:
    - invalid JSON, another board, another project;
    - an older version ("up to date");
    - an image that isn't the manifest's version;
    - a wrong SHA-256, an unsigned image, an image signed with another key
      (both through the updater and through the upload);
    - a download cut off by stopping the server.
  - Rollback: a build that never marks itself valid went back to the
    previous firmware on the next restart. The next check reported it and
    didn't install it automatically.
  - A manual downgrade through the upload.
