#!/usr/bin/env python3
"""Build components/coast.bin, the coastline the ship traffic screen draws,
from the OpenStreetMap coastline data (c) OpenStreetMap contributors, ODbL,
as processed by https://osmdata.openstreetmap.de/data/coastlines.html.

    python3 scripts/build_coast.py [coastlines-split-4326.zip]

Without an argument the zip (~900 MB) is downloaded to ~/.cache/multidisplay.
Only lines inside the region below are kept (Norway's mainland coast and the
neighbouring Swedish/Finnish/Russian coast), joined into continuous lines,
simplified to TOLERANCE_M and cut up into a grid of tiles so the device only
reads the tiles around a location. Islands smaller than MIN_ISLAND_M across
are dropped.

File layout (little-endian; reader: main/main.c, coast_render):
    header : char magic[4] = "CST1", u16 rows, u16 cols,
             i32 lat_min_e5, i32 lon_min_e5, i32 tile_dlat_e5, i32 tile_dlon_e5
    index  : (rows * cols + 1) x u32 - byte offset of each tile's data from the
             start of the data area; tile (r, c) is index r * cols + c, rows
             counted from lat_min northwards; tile t's data ends where t+1's starts
    data   : per tile, a sequence of lines: u16 n, then n x (i16 dlon, i16 dlat)
             in 1e-5 degrees from the tile's south-west corner
Degrees x 1e5 is about 1.1 m. A line's last point may lie just outside its
tile (where it crosses into the next), so every segment is stored exactly once.
"""
import math
import os
import struct
import sys
import urllib.request
import zipfile

import numpy as np

URL = "https://osmdata.openstreetmap.de/download/coastlines-split-4326.zip"
CACHE = os.path.expanduser("~/.cache/multidisplay/coastlines-split-4326.zip")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components", "coast.bin")

LAT_MIN, LAT_MAX = 57.5, 71.5
LON_MIN, LON_MAX = 4.0, 31.5
TILE_DLAT_E5, TILE_DLON_E5 = 10000, 20000   # 0.1 x 0.2 degrees, ~11 x 11 km here
TOLERANCE_M = 40.0
MIN_ISLAND_M = 80.0
MAX_STEP_E5 = 5000                           # keep tile offsets well inside i16


def read_lines(path):
    """Yield each polyline part (N x 2 array of lon, lat) that touches the region."""
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
                if struct.unpack_from("<i", rec, 0)[0] != 3:  # PolyLine
                    continue
                xmin, ymin, xmax, ymax = struct.unpack_from("<4d", rec, 4)
                if xmax < LON_MIN or xmin > LON_MAX or ymax < LAT_MIN or ymin > LAT_MAX:
                    continue
                nparts, npts = struct.unpack_from("<ii", rec, 36)
                parts = list(struct.unpack_from("<%di" % nparts, rec, 44)) + [npts]
                pts = np.frombuffer(rec, dtype="<f8", count=npts * 2, offset=44 + 4 * nparts).reshape(-1, 2)
                for a, b in zip(parts[:-1], parts[1:]):
                    if b - a >= 2:
                        yield pts[a:b].copy()


def join(lines):
    """Chain lines whose end point is another's start point (the split data
    cuts the coast into short pieces; simplifying those separately would pin
    every cut point)."""
    starts = {}
    for i, ln in enumerate(lines):
        starts.setdefault(tuple(ln[0]), []).append(i)
    ends = {tuple(ln[-1]) for ln in lines}
    used = [False] * len(lines)
    out = []
    # Lines nothing leads into first, so chains start at their real beginning;
    # what's left after that are closed rings.
    heads = [i for i, ln in enumerate(lines) if tuple(ln[0]) not in ends]
    for i in heads + list(range(len(lines))):
        if used[i]:
            continue
        used[i] = True
        chain = [lines[i]]
        # Walk forwards.
        while True:
            nxt = next((j for j in starts.get(tuple(chain[-1][-1]), []) if not used[j]), None)
            if nxt is None:
                break
            used[nxt] = True
            chain.append(lines[nxt][1:])
        out.append(np.concatenate(chain))
    return out


def simplify(ln, tol_m):
    """Douglas-Peucker in local metres."""
    lat0 = math.radians(float(np.mean(ln[:, 1])))
    xy = np.column_stack((ln[:, 0] * 111320.0 * math.cos(lat0), ln[:, 1] * 110574.0))
    n = len(xy)
    keep = np.zeros(n, dtype=bool)
    keep[0] = keep[-1] = True
    stack = [(0, n - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        p = xy[a + 1:b]
        d = xy[b] - xy[a]
        length = math.hypot(d[0], d[1])
        if length == 0.0:
            dist = np.hypot(p[:, 0] - xy[a, 0], p[:, 1] - xy[a, 1])
        else:
            dist = np.abs(d[0] * (p[:, 1] - xy[a, 1]) - d[1] * (p[:, 0] - xy[a, 0])) / length
        i = int(np.argmax(dist))
        if dist[i] > tol_m:
            k = a + 1 + i
            keep[k] = True
            stack.append((a, k))
            stack.append((k, b))
    extent = np.ptp(xy, axis=0).max()
    return ln[keep], extent


def densify(e5):
    """Insert points so no step is longer than MAX_STEP_E5 in either axis."""
    out = [e5[0]]
    for p in e5[1:]:
        q = out[-1]
        steps = int(max(abs(p[0] - q[0]), abs(p[1] - q[1])) // MAX_STEP_E5) + 1
        for s in range(1, steps):
            out.append(q + (p - q) * s // steps)
        out.append(p)
    return np.array(out, dtype=np.int64)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else CACHE
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        print("downloading", URL)
        urllib.request.urlretrieve(URL, path)

    raw = list(read_lines(path))
    print("%d line pieces, %d points in the region" % (len(raw), sum(len(l) for l in raw)))
    joined = join(raw)
    print("%d joined lines" % len(joined))

    lat_min_e5 = round(LAT_MIN * 1e5)
    lon_min_e5 = round(LON_MIN * 1e5)
    rows = math.ceil((LAT_MAX - LAT_MIN) * 1e5 / TILE_DLAT_E5)
    cols = math.ceil((LON_MAX - LON_MIN) * 1e5 / TILE_DLON_E5)
    tiles = [[] for _ in range(rows * cols)]

    def tile_of(p):
        r = (p[1] - lat_min_e5) // TILE_DLAT_E5
        c = (p[0] - lon_min_e5) // TILE_DLON_E5
        if 0 <= r < rows and 0 <= c < cols:
            return int(r * cols + c)
        return -1

    kept_pts = 0
    for ln in joined:
        closed = np.array_equal(ln[0], ln[-1])
        s, extent = simplify(ln, TOLERANCE_M)
        if closed and extent < MIN_ISLAND_M:
            continue
        if len(s) < 2:
            continue
        e5 = densify(np.round(s * 1e5).astype(np.int64))
        kept_pts += len(e5)
        # Split into per-tile runs; each run ends with the first point of the
        # next tile so the crossing segment is kept (once).
        cur_t = tile_of(e5[0])
        run = [e5[0]]
        for p in e5[1:]:
            run.append(p)
            t = tile_of(p)
            if t != cur_t:
                if cur_t >= 0:
                    tiles[cur_t].append(run)
                cur_t = t
                run = [p]
        if cur_t >= 0 and len(run) >= 2:
            tiles[cur_t].append(run)
    print("%d points kept after simplifying to %.0f m" % (kept_pts, TOLERANCE_M))

    data = bytearray()
    index = []
    for t, runs in enumerate(tiles):
        index.append(len(data))
        r, c = divmod(t, cols)
        lat0 = lat_min_e5 + r * TILE_DLAT_E5
        lon0 = lon_min_e5 + c * TILE_DLON_E5
        for run in runs:
            for start in range(0, len(run) - 1, 65534):
                part = run[start:start + 65535]
                data += struct.pack("<H", len(part))
                for p in part:
                    data += struct.pack("<hh", int(p[0] - lon0), int(p[1] - lat0))
    index.append(len(data))

    with open(OUT, "wb") as f:
        f.write(b"CST1")
        f.write(struct.pack("<HHiiii", rows, cols, lat_min_e5, lon_min_e5, TILE_DLAT_E5, TILE_DLON_E5))
        f.write(struct.pack("<%dI" % len(index), *index))
        f.write(data)
    print("wrote %s: %d x %d tiles, %d bytes" % (os.path.normpath(OUT), rows, cols, os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
