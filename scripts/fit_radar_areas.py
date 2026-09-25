#!/usr/bin/env python3
"""Work out where the pixels of MET Norway's radar images lie, for the rain
radar screen (components/rain_client.c, AREAS).

    python3 scripts/fit_radar_areas.py [area ...]

api.met.no/weatherapi/radar/2.0 serves finished map images per area, drawn in
a Lambert conformal conic projection whose parameters aren't published (the
bounding boxes in its locations.json don't match the images). This fits, per
area, a spherical LCC (standard parallel lat1, central meridian lon0) and the
image grid on it (metres per pixel, offset) in two steps. First it lines up
components/coast.bin with the sea in the image: coastline points should have
sea on their left (coast.bin's convention) and land on their right; a coarse
search over lon0 and scale finds the offset by FFT cross-correlation, then a
local search refines all five. That gets within a few pixels, and then the
dotted lat/lon lines drawn on the image (pure #323232 pixels) pin it down:
a simplex search moves the fit until they lie on whole degrees. Prints the C table to paste into rain_client.c, and writes
fit_<area>.png overlays (coastline in red) to the current directory to check.

Needs numpy and Pillow. Takes a few minutes per area.
"""
import math
import os
import struct
import sys
import urllib.request
import io

import numpy as np
from PIL import Image

UA = "MultiDisplay radar fit github.com/impytv/MultiDisplay"
URL = "https://api.met.no/weatherapi/radar/2.0/?area=%s&type=5level_reflectivity"
COAST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components", "coast.bin")
AREAS = ["southeastern_norway", "eastern_norway", "southern_norway", "southwestern_norway",
         "western_norway", "northwestern_norway", "central_norway", "southern_nordland",
         "nordland", "northern_nordland", "troms", "finnmark", "norway", "nordic"]
LEGEND_W = 90
R = 6371000.0


def load_coast(path):
    """coast.bin (see scripts/build_coast.py) as a list of N x 2 (lon, lat) arrays."""
    b = open(path, "rb").read()
    rows, cols, lat0, lon0, dlat, dlon, unit = struct.unpack_from("<HHiiiii", b, 4)
    n = rows * cols + 1
    idx = struct.unpack_from("<%dI" % n, b, 28)
    data = 28 + 4 * n
    lines = []

    def varint(p):
        v = s = 0
        while True:
            c = b[p]
            p += 1
            v |= (c & 0x7F) << s
            s += 7
            if not c & 0x80:
                return v, p

    for t in range(rows * cols):
        p, end = data + idx[t], data + idx[t + 1]
        r, c = divmod(t, cols)
        cur = [lon0 + c * dlon, lat0 + r * dlat]
        while p < end:
            k, p = varint(p)
            pts = []
            for _ in range(k):
                for j in range(2):
                    z, p = varint(p)
                    cur[j] += ((z >> 1) ^ -(z & 1)) * unit
                pts.append((cur[0] / 1e5, cur[1] / 1e5))
            lines.append(np.array(pts))
    return lines


LINES = load_coast(COAST)
A = np.concatenate([l[:-1] for l in LINES])
B = np.concatenate([l[1:] for l in LINES])
MID = (A + B) / 2


def proj(lat1, lon0, lon, lat):
    """Spherical LCC, x east and y north of the cone apex, metres."""
    p1 = math.radians(lat1)
    n = math.sin(p1)
    f = math.cos(p1) * math.tan(math.pi / 4 + p1 / 2) ** n / n
    rho = R * f / np.tan(np.pi / 4 + np.radians(lat) / 2) ** n
    th = n * np.radians(lon - lon0)
    return rho * np.sin(th), -rho * np.cos(th)


def pix(p, lon, lat):
    lat1, lon0, s, ox, oy = p
    x, y = proj(lat1, lon0, np.asarray(lon, float), np.asarray(lat, float))
    return x / s - ox, -y / s - oy


def inv_pix(p, px, py):
    lon, lat = 15.0, 63.0
    for _ in range(50):
        x, y = pix(p, lon, lat)
        x2, y2 = pix(p, lon + 0.01, lat)
        x3, y3 = pix(p, lon, lat + 0.01)
        J = np.array([[x2 - x, x3 - x], [y2 - y, y3 - y]]) / 0.01
        d = np.linalg.solve(J, [px - x, py - y])
        lon += d[0]
        lat += d[1]
    return lon, lat


def sea_mask(im):
    """+1 sea (and lakes), -1 land, 0 anything else (rain, text, lines, legend)."""
    a = np.asarray(im.convert("RGB")).astype(int)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    sat = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    m = np.zeros(r.shape)
    m[(b - r > 10) & (b - g > 10) & (r > 80)] = 1
    m[(np.abs(b - r) <= 9) & (g >= r) & (sat < 15) & (r > 95)] = -1
    m[:, m.shape[1] - LEGEND_W:] = 0
    return m


def blur(m, k=1.5):
    r = int(3 * k)
    x = np.arange(-r, r + 1)
    g = np.exp(-x * x / (2 * k * k))
    g /= g.sum()
    m = np.apply_along_axis(lambda v: np.convolve(v, g, "same"), 0, m)
    return np.apply_along_axis(lambda v: np.convolve(v, g, "same"), 1, m)


def samples(lat1, lon0, s):
    """Coastline segment middles and their water-side normals, image-like coordinates (y down)."""
    ax, ay = proj(lat1, lon0, A[:, 0], A[:, 1])
    bx, by = proj(lat1, lon0, B[:, 0], B[:, 1])
    ax, ay, bx, by = ax / s, -ay / s, bx / s, -by / s
    dx, dy = bx - ax, by - ay
    ln = np.hypot(dx, dy) + 1e-9
    return (ax + bx) / 2, (ay + by) / 2, dy / ln, -dx / ln


def best_offset(m, mx, my, nx, ny, d=2.5):
    h, w = m.shape
    xs = np.concatenate([mx + nx * d, mx - nx * d])
    ys = np.concatenate([my + ny * d, my - ny * d])
    v = np.concatenate([np.ones(len(mx)), -np.ones(len(mx))])
    x0, y0 = np.floor(xs.min()) - w, np.floor(ys.min()) - h
    W = int(xs.max() - x0) + w + 2
    H = int(ys.max() - y0) + h + 2
    Wf = 1 << int(math.ceil(math.log2(W)))
    Hf = 1 << int(math.ceil(math.log2(H)))
    T = np.zeros((Hf, Wf))
    np.add.at(T, (np.round(ys - y0).astype(int), np.round(xs - x0).astype(int)), v)
    M = np.zeros((Hf, Wf))
    M[:h, :w] = m
    C = np.fft.irfft2(np.fft.rfft2(T) * np.conj(np.fft.rfft2(M)), s=(Hf, Wf))
    iy, ix = np.unravel_index(np.argmax(C), C.shape)
    return ix + x0, iy + y0


def bil(m, x, y):
    h, w = m.shape
    ok = (x >= 0) & (x < w - 1) & (y >= 0) & (y < h - 1)
    x, y = x[ok], y[ok]
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    v = (m[y0, x0] * (1 - fx) * (1 - fy) + m[y0, x0 + 1] * fx * (1 - fy) +
         m[y0 + 1, x0] * (1 - fx) * fy + m[y0 + 1, x0 + 1] * fx * fy)
    out = np.zeros(len(ok))
    out[ok] = v
    return out, ok


def score(mb, p, d=3.0):
    lat1, lon0, s, ox, oy = p
    mx, my, nx, ny = samples(lat1, lon0, s)
    a, ok1 = bil(mb, mx + nx * d - ox, my + ny * d - oy)
    b, ok2 = bil(mb, mx - nx * d - ox, my - ny * d - oy)
    ok = ok1 & ok2
    return (a - b)[ok].sum() / max(ok.sum(), 1)


def anchored(p, q, anchor):
    """New lat1, lon0, scale q with the offset set so `anchor` stays on the same pixel."""
    ax, ay = pix(p, *anchor)
    lat1, lon0, s = q
    x, y = proj(lat1, lon0, np.float64(anchor[0]), np.float64(anchor[1]))
    return [lat1, lon0, s, x / s - ax, -y / s - ay]


def refine(mb, p, anchor):
    p = list(p)
    best = score(mb, p)
    for st in [2, 1, 0.5, 0.25, 0.12, 0.06, 0.03]:
        delta = [st * 4, st, st * 8, st, st]
        improved = True
        while improved:
            improved = False
            for i in range(5):
                for sg in (1, -1):
                    if i < 3:
                        q = [p[0], p[1], p[2]]
                        q[i] += sg * delta[i]
                        q = anchored(p, q, anchor)
                    else:
                        q = p[:]
                        q[i] += sg * delta[i]
                    sc = score(mb, q)
                    if sc > best + 1e-7:
                        best, p, improved = sc, q, True
    return p, best


def inv(p, px, py):
    """Image pixel to (lat, lon), vectorised."""
    lat1, lon0, s, ox, oy = p
    p1 = math.radians(lat1)
    n = math.sin(p1)
    f = math.cos(p1) * math.tan(math.pi / 4 + p1 / 2) ** n / n
    x, yd = (px + ox) * s, (py + oy) * s
    rho, th = np.hypot(x, yd), np.arctan2(x, yd)
    return np.degrees(2 * np.arctan((R * f / rho) ** (1 / n)) - np.pi / 2), lon0 + np.degrees(th / n)


def grid_dist(p, xs, ys):
    """Distance (px) from each pixel to the nearest whole-degree meridian or parallel."""
    lat, lon = inv(p, xs, ys)
    dlon = np.abs(lon - np.round(lon)) * 111320 * np.cos(np.radians(lat)) / p[2]
    dlat = np.abs(lat - np.round(lat)) * 110574 / p[2]
    return np.minimum(dlon, dlat)


def simplex(f, x0, scale, iters=2000):
    """Plain Nelder-Mead."""
    pts = [np.array(x0, float)]
    for i in range(len(x0)):
        q = np.array(x0, float)
        q[i] += scale[i]
        pts.append(q)
    vals = [f(q) for q in pts]
    for _ in range(iters):
        o = np.argsort(vals)
        pts, vals = [pts[i] for i in o], [vals[i] for i in o]
        c = np.mean(pts[:-1], axis=0)
        xr = c + (c - pts[-1])
        fr = f(xr)
        if fr < vals[0]:
            xe = c + 2 * (c - pts[-1])
            fe = f(xe)
            pts[-1], vals[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            xc = c + 0.5 * (pts[-1] - c)
            fc = f(xc)
            if fc < vals[-1]:
                pts[-1], vals[-1] = xc, fc
            else:
                for i in range(1, len(pts)):
                    pts[i] = pts[0] + 0.5 * (pts[i] - pts[0])
                    vals[i] = f(pts[i])
    return list(pts[int(np.argmin(vals))])


def polish(im, p):
    a = np.asarray(im.convert("RGB")).astype(int)
    m = (a[..., 0] == 50) & (a[..., 1] == 50) & (a[..., 2] == 50)
    m[:, m.shape[1] - LEGEND_W:] = False
    ys, xs = np.nonzero(m)
    xs, ys = xs.astype(float), ys.astype(float)
    cost = lambda q: np.mean(np.minimum(grid_dist(q, xs, ys), 2.0) ** 2)
    p = simplex(cost, p, [0.5, 0.3, 2.0, 1.0, 1.0])
    p = simplex(cost, p, [0.1, 0.05, 0.5, 0.3, 0.3])
    return p, float(np.median(grid_dist(p, xs, ys)))


def fit(area):
    req = urllib.request.Request(URL % area, headers={"User-Agent": UA})
    im = Image.open(io.BytesIO(urllib.request.urlopen(req).read()))
    w, h = im.size
    m = sea_mask(im)
    mb = blur(m)
    best = None
    for s in (1000.0, 1500.0, 2000.0, 2500.0, 3000.0):
        for lon0 in np.arange(0, 31, 1.0):
            mx, my, nx, ny = samples(63, lon0, s)
            ox, oy = best_offset(m, mx, my, nx, ny)
            sc = score(mb, (63, lon0, s, ox, oy))
            if best is None or sc > best[0]:
                best = (sc, [63, lon0, s, ox, oy])
    p = best[1]
    p, sc = refine(mb, p, inv_pix(p, w / 2, h / 2))
    p, med = polish(im, p)
    ov = im.convert("RGB")
    px = ov.load()
    x, y = pix(p, MID[:, 0], MID[:, 1])
    for a, b in zip(x, y):
        if 0 <= a < w and 0 <= b < h:
            px[int(a), int(b)] = (255, 0, 0)
    ov.save("fit_%s.png" % area)
    return w, h, p, med


def main():
    for area in sys.argv[1:] or AREAS:
        try:
            w, h, p, med = fit(area)
        except Exception as e:  # e.g. an area that's gone
            print("/* %s: %s */" % (area, e), flush=True)
            continue
        lat1, lon0, s, ox, oy = p
        print('    { "%s", %d, %d, %.3ff, %.3ff, %.2ff, %.2ff, %.2ff }, /* grid lines within %.2f px */'
              % (area, w, h, lat1, lon0, s, ox, oy, med), flush=True)


if __name__ == "__main__":
    main()
