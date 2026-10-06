#!/usr/bin/env python3
"""Build components/sat_palette.bin, the colours the satellite screen keeps
its picture of Europe in (main/satellite.c).

    python3 scripts/build_sat_palette.py

The Europe view is held as one byte per pixel - an index into 256 colours -
which halves its memory against RGB565. MET's images keep to a narrow range
of colours (the infrared one is drawn with a fixed colour scale, the visible
one is mostly cloud, sea and land), so one palette per kind, fitted to
recent images, does as well as RGB565 without dithering (about 38-41 dB
PSNR either way). This fits them: the images of the last two days are
shrunk as on the display, a median cut gives a start and k-means refines
it.

File layout, for each kind (infrared, then visible):
    palette: 256 x 4 bytes, as LVGL's lv_color32_t (blue, green, red, 255)
    lookup : 65536 bytes, the nearest palette entry for each RGB565 value

Needs numpy and Pillow. Takes a few minutes.
"""
import datetime
import io
import os
import urllib.request

import numpy as np
from PIL import Image

UA = "MultiDisplay palette github.com/impytv/MultiDisplay"
URL = "https://api.met.no/weatherapi/geosatellite/1.4/?area=europe&type=%s&time=%s"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "components", "sat_palette.bin")
HOURS = {"infrared": range(0, 24, 4), "visible": (8, 10, 12, 14)}  # UTC; visible only by day


def image(kind, t):
    req = urllib.request.Request(URL % (kind, t.strftime("%Y-%m-%dT%H:%M:00Z")), headers={"User-Agent": UA})
    try:
        data = urllib.request.urlopen(req, timeout=60).read()
    except Exception as e:  # older than MET keeps, or missing
        print("  %s %s: %s" % (kind, t, e))
        return None
    return np.asarray(Image.open(io.BytesIO(data)).convert("RGB").resize((800, 450), Image.BOX))


def fit(images, n=256, rounds=10):
    start = Image.fromarray(np.concatenate(images, 0)).quantize(n, method=Image.Quantize.MEDIANCUT,
                                                               dither=Image.Dither.NONE)
    c = np.array(start.getpalette()[:3 * n], float).reshape(-1, 3)
    px = np.concatenate([i.reshape(-1, 3) for i in images])[::5].astype(float)
    for _ in range(rounds):
        idx = np.concatenate([((px[s:s + 20000, None, :] - c[None]) ** 2).sum(2).argmin(1)
                              for s in range(0, len(px), 20000)])
        for k in range(n):
            m = idx == k
            if m.any():
                c[k] = px[m].mean(0)
    return np.clip(np.round(c), 0, 255).astype(np.uint8)


def lookup(pal):
    """The nearest entry for every RGB565 value, taken as the device widens it."""
    v = np.arange(65536)
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    rgb = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], 1).astype(float)
    p = pal.astype(float)
    return np.concatenate([((rgb[s:s + 4096, None, :] - p[None]) ** 2).sum(2).argmin(1)
                           for s in range(0, 65536, 4096)]).astype(np.uint8)


def main():
    now = datetime.datetime.now(datetime.timezone.utc).replace(minute=0, second=0, microsecond=0)
    out = bytearray()
    for kind in ("infrared", "visible"):
        images = []
        for day in (2, 1):
            for h in HOURS[kind]:
                t = (now - datetime.timedelta(days=day)).replace(hour=h)
                im = image(kind, t)
                if im is not None:
                    images.append(im)
        print("%s: %d images" % (kind, len(images)))
        pal = fit(images)
        bgra = np.concatenate([pal[:, ::-1], np.full((256, 1), 255, np.uint8)], 1)
        out += bgra.tobytes() + lookup(pal).tobytes()
    with open(OUT, "wb") as f:
        f.write(out)
    print("wrote %s (%d bytes)" % (OUT, len(out)))


if __name__ == "__main__":
    main()
