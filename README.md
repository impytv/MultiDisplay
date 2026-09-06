| Supported Targets | ESP32-S3 |
| ----------------- | -------- |

# LVGL9 Adapter Demo

This example targets the Waveshare `ESP32-S3-Touch-LCD-4.3B` board and runs the official
`lv_demo_widgets()` demo with:

- `LVGL 9`
- `espressif/esp_lvgl_adapter`
- RGB panel output
- GT911 touch input

## Requirements

- ESP-IDF `>= 5.5`
- Internet access on the first build so the component manager can download dependencies

## Build

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## Notes

- The example keeps the existing `4.3B` RGB, CH422G and GT911 bring-up flow, and only replaces the LVGL porting layer with `esp_lvgl_adapter`.
- The default panel resolution is `800x480`.
- Touch is enabled by default. If your panel variant has no touch, set `EXAMPLE_USE_TOUCH` to `0` in `main/waveshare_rgb_lcd_port.h`.

## YR weather (MET Norway)

The "YR" tab connects to WiFi and shows the current forecast from the
[MET Norway Locationforecast API](https://developer.yr.no/doc/), refreshed
every 10 minutes.

### Multiple locations

Up to 5 forecast locations can be stored. **Tap the left half of the screen**
to cycle through the stops:

1. **Oversikt** – an overview table with one row per location and 6-hour
   columns, each showing the weather icon, temperature and the precipitation
   summed over that 6-hour block. This is the first stop.
2. One detail screen per location (chart + wind), in order. The top-left
   label shows e.g. `Oslo  2/3`.

The overview stop only appears when two or more locations are configured.
All locations' hourly forecasts are kept refreshed in the background so the
table is always current; the detail screens additionally splice in the
5-minute nowcast for the selected location.

### Setup portal (WiFi + locations)

WiFi credentials and the forecast locations are configured at runtime and
stored in NVS — no rebuild needed to change them.

On a fresh flash (WiFi SSID still the `myssid` placeholder in `sdkconfig`),
or any time you **hold the BOOT button while powering on**, or if the saved
WiFi fails to connect, the device starts a setup access point:

1. Connect a phone/laptop to the WiFi network **`MultiDisplay-XXXX`** (open).
2. A "sign in to network" page opens automatically (captive portal); if not,
   browse to **`http://192.168.4.1/`**.
3. Pick your WiFi network, enter the password, then fill in one or more
   **Location** blocks (name + latitude/longitude). Leave a block empty to
   skip it. Press **Save** — the device reboots and connects.

Once connected, the same page is reachable at the device's IP on your LAN
(shown in the router's client list, or the serial log: `Got IP: …`) for
later edits.

### Build-time seed defaults

The values under `idf.py menuconfig` → `MultiDisplay Configuration` only
pre-fill the portal form (and let you skip the portal by setting a real
`WiFi SSID`). The **YR API User-Agent** is still build-time only and must be
set — api.met.no's
[Terms of Service](https://developer.yr.no/doc/TermsOfService/) reject a
missing/generic User-Agent with `403 Forbidden`; use something like
`MyDevice/1.0 myname@example.com`.

`sdkconfig` is **not committed** (see `.gitignore`); `sdkconfig.defaults`
holds the shared build settings and regenerates a fresh `sdkconfig` on first
build.

The "Fly" and "Regn" tabs are placeholders for future features.
