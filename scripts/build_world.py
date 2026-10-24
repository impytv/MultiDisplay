#!/usr/bin/env python3
"""Build components/world.bin, the world map under the ISS screen
(main/iss.c), from Natural Earth's 1:50m land polygons less its 1:110m
lakes - the largest ones (public domain, https://www.naturalearthdata.com/).

    python3 scripts/build_world.py [ne_50m_land.zip ne_110m_lakes.zip]

Without arguments the zips (~460 KB) are downloaded to ~/.cache/multidisplay.

The map is the whole world in the plate carree projection (longitude and
latitude straight to x and y), W x H pixels from 180 W, 90 N at the top
left. Each pixel holds how much of it is land, 0 (sea) to 3 (land), so
the coast can be drawn smoothly: found by drawing the polygons at SS times
the size and counting. 2 bits a pixel, four pixels to a byte, the leftmost
in the top bits, rows from the top: W * H / 4 bytes, no header.
"""
import os
import struct
import sys
import urllib.request
import zipfile

import numpy as np

URL = "https://naciscdn.org/naturalearth/%s/physical/%s.zip"
CACHE = os.path.expanduser("~/.cache/multidisplay")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components", "world.bin")

W, H = 800, 400
SS = 4


def rings(path):
    """Yield every ring of every polygon, as an N x 2 array of lon, lat."""
    with zipfile.ZipFile(path) as z:
        name = next(n for n in z.namelist() if n.endswith(".shp"))
        with z.open(name) as f:
            f.read(100)  # file header
            while True:
                hdr = f.read(8)
                if len(hdr) < 8:
                    return
                _, words = struct.unpack(">ii", hdr)
                rec = f.read(words * 2)
                if struct.unpack_from("<i", rec, 0)[0] != 5:  # Polygon
                    continue
                nparts, npts = struct.unpack_from("<ii", rec, 36)
                parts = list(struct.unpack_from("<%di" % nparts, rec, 44)) + [npts]
                pts = np.frombuffer(rec, dtype="<f8", count=npts * 2, offset=44 + 4 * nparts).reshape(-1, 2)
                for a, b in zip(parts[:-1], parts[1:]):
                    if b - a >= 3:
                        yield pts[a:b]


def fill(mask, ring, value):
    """Set the pixels whose centres are inside `ring` to `value`: for each
    row, where the ring's edges cross it, and between every other pair of
    crossings. (PIL's polygon fill left rows out of large polygons.)"""
    h, w = mask.shape
    x = (ring[:, 0] + 180.0) * w / 360.0
    y = (90.0 - ring[:, 1]) * h / 180.0
    xa, ya, xb, yb = x, y, np.roll(x, -1), np.roll(y, -1)
    lo, hi = np.minimum(ya, yb), np.maximum(ya, yb)
    for row in range(max(int(np.floor(y.min())), 0), min(int(np.ceil(y.max())) + 1, h)):
        c = row + 0.5
        k = (lo <= c) & (hi > c)  # half-open: a corner on the row counts once
        if not k.any():
            continue
        xs = np.sort(xa[k] + (c - ya[k]) * (xb[k] - xa[k]) / (yb[k] - ya[k]))
        for x0, x1 in zip(xs[0::2], xs[1::2]):
            i0, i1 = max(int(np.ceil(x0 - 0.5)), 0), min(int(np.floor(x1 - 0.5)), w - 1)
            if i1 >= i0:
                mask[row, i0:i1 + 1] = value


def clockwise(ring):
    """A shapefile's outer rings go clockwise, its holes the other way."""
    x, y = ring[:, 0], ring[:, 1]
    return np.sum(x[:-1] * y[1:] - x[1:] * y[:-1]) < 0


def source(scale, name, arg):
    if arg:
        return arg
    path = os.path.join(CACHE, name + ".zip")
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        print("Downloading", URL % (scale, name))
        urllib.request.urlretrieve(URL % (scale, name), path)
    return path


def main():
    args = sys.argv[1:] + [None, None]
    land_zip = source("50m", "ne_50m_land", args[0])
    lakes_zip = source("110m", "ne_110m_lakes", args[1])

    # The land, then its holes and the lakes taken out.
    w, h = W * SS, H * SS
    land = np.zeros((h, w), dtype=bool)
    all_rings = list(rings(land_zip))
    for ring in all_rings:
        if clockwise(ring):
            fill(land, ring, True)
    for ring in all_rings:
        if not clockwise(ring):
            fill(land, ring, False)
    n = len(all_rings)
    for ring in rings(lakes_zip):
        fill(land, ring, False)
        n += 1

    cover = land.reshape(H, SS, W, SS).sum(axis=(1, 3))  # 0 .. SS*SS
    level = np.rint(cover * 3.0 / (SS * SS)).astype(np.uint8)
    packed = (level[:, 0::4] << 6) | (level[:, 1::4] << 4) | (level[:, 2::4] << 2) | level[:, 3::4]
    with open(OUT, "wb") as f:
        f.write(packed.astype(np.uint8).tobytes())
    print("%d rings, %.1f%% land, %d bytes -> %s" % (n, 100.0 * land.mean(), packed.size, os.path.normpath(OUT)))


if __name__ == "__main__":
    main()
