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

### Setup portal (WiFi + location)

WiFi credentials and the forecast location are configured at runtime and
stored in NVS — no rebuild needed to change them.

On a fresh flash (WiFi SSID still the `myssid` placeholder in `sdkconfig`),
or any time you **hold the BOOT button while powering on**, or if the saved
WiFi fails to connect, the device starts a setup access point:

1. Connect a phone/laptop to the WiFi network **`MultiDisplay-XXXX`** (open).
2. A "sign in to network" page opens automatically (captive portal); if not,
   browse to **`http://192.168.4.1/`**.
3. Pick your WiFi network, enter the password, set the location name and
   latitude/longitude, and **Save**. The device reboots and connects.

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
