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

Before building, set your WiFi credentials, forecast coordinates, and a
User-Agent identifying your app (required by api.met.no's
[Terms of Service](https://developer.yr.no/doc/TermsOfService/) or requests
get a `403 Forbidden`):

```bash
idf.py menuconfig
```

under `HelloESPLVGLClaude Configuration`:

- `WiFi SSID` / `WiFi password`
- `YR forecast latitude` / `longitude` (default: Oslo, `59.91`/`10.75`)
- `YR API User-Agent` — replace the default placeholder with something like
  `MyDevice/1.0 myname@example.com`

These end up in `sdkconfig`, which is **not committed** (see `.gitignore`) -
each clone/checkout sets its own via menuconfig. `sdkconfig.defaults` holds
the shared, non-secret build settings and is regenerated into a fresh
`sdkconfig` automatically on first build.

The "Fly" and "Regn" tabs are placeholders for future features.
