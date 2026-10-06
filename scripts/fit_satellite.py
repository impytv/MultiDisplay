#!/usr/bin/env python3
"""Work out where the pixels of MET Norway's satellite image of Europe lie,
for the satellite screen's close-ups (components/sat_util.c, SAT_*).

    python3 scripts/fit_satellite.py

api.met.no/weatherapi/geosatellite/1.4 serves finished 1280 x 720 images
whose projection isn't documented. They turn out to be close to a spherical
Lambert conformal conic projection, so this fits one (standard parallel
lat1, central meridian lon0, pixels per Earth radius, and the pixel where
the two cross) to the coastline drawn on the image.

The coastline is taken from a visible-light image from the middle of the
night: everything is black but the lines. The Natural Earth 1:50m coastline
(public domain) is then moved onto it: a coarse search, then a simplex
search on the mean distance from each coastline point to the nearest drawn
line. Prints the #defines to paste into sat_util.c, and writes
fit_satellite.png (the coastline in red over the image) to the current
directory to check by eye - over Norway it should lie within a pixel.

Needs numpy and Pillow. Takes a few minutes.
"""
import datetime
import io
import json
import math
import urllib.request

import numpy as np
from PIL import Image, ImageFilter

UA = "MultiDisplay satellite fit github.com/impytv/MultiDisplay"
URL = "https://api.met.no/weatherapi/geosatellite/1.4/?area=europe&type=visible&time=%sT00:00:00Z"
COAST = ("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/"
         "ne_50m_coastline.geojson")
D2R = math.pi / 180
CLIP = 12.0


def fetch(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=60).read()


def line_mask(img):
    """The drawn lines: lavender grey (r == g, b a bit higher) on black."""
    a = np.asarray(img.convert("RGB")).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    return (abs(r - g) <= 2) & (b > r * 1.12) & (r > 12)


def distance_map(mask, steps=40):
    """Chessboard distance (px) to the nearest line, up to `steps`."""
    dt = np.full(mask.shape, float(steps))
    cur = Image.fromarray((mask * 255).astype("uint8"))
    for d in range(steps):
        a = np.asarray(cur) > 0
        dt[a & (dt == steps)] = d
        cur = cur.filter(ImageFilter.MaxFilter(3))
    return dt


def coastline():
    pts = []
    for f in json.loads(fetch(COAST))["features"]:
        geo = f["geometry"]
        lines = [geo["coordinates"]] if geo["type"] == "LineString" else geo["coordinates"]
        for line in lines:
            pts += [c for c in line if -60 < c[0] < 60 and 25 < c[1] < 85]
    p = np.array(pts)
    return p[:, 0], p[:, 1]


def lcc(p, lon, lat):
    """As sat_project() in sat_util.c."""
    lat1, lon0, k, cx, cy = p
    p1 = lat1 * D2R
    n = math.sin(p1)
    f = math.cos(p1) * math.tan(math.pi / 4 + p1 / 2) ** n / n
    rho = f / np.tan(math.pi / 4 + lat * D2R / 2) ** n
    rho0 = f / math.tan(math.pi / 4 + p1 / 2) ** n
    th = n * (lon - lon0) * D2R
    return cx + k * rho * np.sin(th), cy - k * (rho0 - rho * np.cos(th))


def simplex(f, x0, step, iters):
    n = len(x0)
    s = [np.array(x0, float)]
    for i in range(n):
        v = np.array(x0, float)
        v[i] += step[i]
        s.append(v)
    fs = [f(v) for v in s]
    for _ in range(iters):
        o = np.argsort(fs)
        s = [s[i] for i in o]
        fs = [fs[i] for i in o]
        c = np.mean(s[:-1], 0)
        xr = c + (c - s[-1])
        fr = f(xr)
        if fr < fs[0]:
            xe = c + 2 * (c - s[-1])
            fe = f(xe)
            s[-1], fs[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < fs[-2]:
            s[-1], fs[-1] = xr, fr
        else:
            xc = c + 0.5 * (s[-1] - c)
            fc = f(xc)
            if fc < fs[-1]:
                s[-1], fs[-1] = xc, fc
            else:
                s = [s[0] + 0.5 * (v - s[0]) for v in s]
                fs = [f(v) for v in s]
    i = int(np.argmin(fs))
    return s[i], fs[i]


def main():
    day = (datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=1)).strftime("%Y-%m-%d")
    img = Image.open(io.BytesIO(fetch(URL % day)))
    w, h = img.size
    print("image %s at midnight UTC: %dx%d" % (day, w, h))
    dt = distance_map(line_mask(img))
    lon, lat = coastline()

    def cost(p):
        with np.errstate(all="ignore"):
            x, y = lcc(p, lon, lat)
        ok = np.isfinite(x) & np.isfinite(y) & (x >= 0) & (x < w - 1) & (y >= 0) & (y < h - 1)
        c = np.full(len(lon), CLIP)
        c[ok] = np.minimum(dt[y[ok].astype(int), x[ok].astype(int)], CLIP)
        return c.mean()

    # Coarse: (55N, 10E) - Denmark - near (780, 300), then the rest.
    best = None
    for lat1 in range(30, 90, 10):
        for lon0 in range(-30, 31, 10):
            for k in np.linspace(400, 1600, 7):
                x, y = lcc([lat1, lon0, k, 0, 0], np.array([10.0]), np.array([55.0]))
                for dx in (-80, 0, 80):
                    for dy in (-80, 0, 80):
                        p = [lat1, lon0, k, 780 + dx - x[0], 300 + dy - y[0]]
                        c = cost(p)
                        if best is None or c < best[1]:
                            best = (p, c)
    p, c = simplex(cost, best[0], [5, 5, 50, 15, 15], 400)
    p, c = simplex(cost, p, [0.5, 0.5, 5, 2, 2], 400)
    print("mean distance %.2f px (clipped at %g)" % (c, CLIP))
    for name, v in zip(("LAT1", "LON0", "K", "CX", "CY"), p):
        print("#define SAT_%-5s %.5f" % (name, v))

    x, y = lcc(p, lon, lat)
    ok = (x >= 0) & (x < w - 1) & (y >= 0) & (y < h - 1)
    a = np.array(img.convert("RGB"))
    a[y[ok].astype(int), x[ok].astype(int)] = [255, 0, 0]
    Image.fromarray(a).save("fit_satellite.png")


if __name__ == "__main__":
    main()
