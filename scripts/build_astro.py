#!/usr/bin/env python3
"""Build components/astro_data.h, the astronomical events on the month
calendar (main/month.c, components/astro.c), with Skyfield and JPL's DE440s
ephemeris:

  - the moon's phases (new, first quarter, full, last quarter), and which
    full moons are supermoons (closer than SUPERMOON_KM);
  - the equinoxes and solstices;
  - the peaks of the main meteor showers, from the sun's longitude at their
    peak (IMO), on the evening the night of the peak begins;
  - lunar eclipses with an umbral phase (partial or total; penumbral ones
    are hard to see), at greatest eclipse;
  - solar eclipses anywhere on Earth, as the sun's and moon's geocentric
    positions every SOLAR_STEP_S over the eclipse - the display works out
    from these whether, and how much of, the sun is covered where it is.

    pip install skyfield
    curl -O https://ssd.jpl.nasa.gov/ftp/eph/planets/bsp/de440s.bsp
    scripts/build_astro.py path/to/de440s.bsp [first_year [last_year]]

Years default to this one and the nine after it. Prints the largest share
of the sun covered in Oslo for each solar eclipse, as a check against the
display's own reckoning (test/host/test_astro.c).
"""

import datetime
import math
import os
import sys

from skyfield import almanac, eclipselib
from skyfield.api import load, load_file, wgs84
from skyfield.framelib import ecliptic_J2000_frame
from skyfield.searchlib import find_discrete

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components", "astro_data.h")
SUPERMOON_KM = 360000
SOLAR_STEP_S = 600
SOLAR_HALF_S = 4 * 3600  # samples from this long before to after the closest approach
# Peaks by the sun's apparent ecliptic longitude (degrees, J2000; IMO's
# working list), and the name shown.
SHOWERS = [
    (283.15, "Kvadrantidene"),
    (32.32, "Lyridene"),
    (140.0, "Perseidene"),
    (208.0, "Orionidene"),
    (235.27, "Leonidene"),
    (262.2, "Geminidene"),
]
OSLO = (59.9139, 10.7522)

PHASE_TYPE = ["ASTRO_NEW_MOON", "ASTRO_FIRST_QUARTER", "ASTRO_FULL_MOON", "ASTRO_LAST_QUARTER"]
SEASON_TYPE = ["ASTRO_MARCH_EQUINOX", "ASTRO_JUNE_SOLSTICE", "ASTRO_SEPTEMBER_EQUINOX", "ASTRO_DECEMBER_SOLSTICE"]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    eph = load_file(sys.argv[1])
    first = int(sys.argv[2]) if len(sys.argv) > 2 else datetime.date.today().year
    last = int(sys.argv[3]) if len(sys.argv) > 3 else first + 9
    ts = load.timescale()
    t0 = ts.utc(first, 1, 1)
    t1 = ts.utc(last + 1, 1, 1)
    earth, sun, moon = eph["earth"], eph["sun"], eph["moon"]
    events = []  # (unix seconds, type, arg)

    def unix(t):
        return int(round(t.utc_datetime().timestamp()))

    times, phases = almanac.find_discrete(t0, t1, almanac.moon_phases(eph))
    for t, p in zip(times, phases):
        events.append((unix(t), PHASE_TYPE[p], 0))
        if p == 2:
            km = earth.at(t).observe(moon).distance().km
            if km < SUPERMOON_KM:
                events.append((unix(t), "ASTRO_SUPERMOON", 0))

    times, seasons = almanac.find_discrete(t0, t1, almanac.seasons(eph))
    for t, s in zip(times, seasons):
        events.append((unix(t), SEASON_TYPE[s], 0))

    for i, (lon, _) in enumerate(SHOWERS):
        def past(t, lon=lon):
            _, elon, _ = earth.at(t).observe(sun).apparent().frame_latlon(ecliptic_J2000_frame)
            return ((elon.degrees - lon) % 360) < 180
        past.step_days = 30
        for t, v in zip(*find_discrete(ts.utc(first - 1, 12, 1), t1, past)):
            if v and first <= t.utc_datetime().year <= last:
                events.append((unix(t), "ASTRO_METEORS", i))

    times, kinds, _ = eclipselib.lunar_eclipses(t0, t1, eph)
    for t, k in zip(times, kinds):
        if k >= 1:  # partial or total
            events.append((unix(t), "ASTRO_LUNAR_ECLIPSE", int(k)))

    # Solar eclipses: new moons where the moon passes within reach of the
    # sun's disc from somewhere on Earth.
    tracks = []
    times, phases = almanac.find_discrete(t0, t1, almanac.moon_phases(eph))
    for t, p in zip(times, phases):
        if p != 0:
            continue
        # The closest geocentric approach, to the minute, within 6 h.
        best = None
        for m in range(-360, 361, 2):
            tm = ts.tt_jd(t.tt + m / 1440)
            e = earth.at(tm)
            sep = e.observe(sun).apparent().separation_from(e.observe(moon).apparent()).degrees
            if best is None or sep < best[0]:
                best = (sep, tm)
        sep, tc = best
        e = earth.at(tc)
        md = e.observe(moon).distance().km
        sd = e.observe(sun).distance().km
        reach = math.degrees(math.asin(6378.14 / md) + math.asin(1737.4 / md) + math.asin(696000 / sd))
        if sep > reach:
            continue
        samples = []
        for s in range(-SOLAR_HALF_S, SOLAR_HALF_S + 1, SOLAR_STEP_S):
            ti = ts.tt_jd(tc.tt + s / 86400)
            ei = earth.at(ti)
            sra, sdec, sdist = ei.observe(sun).apparent().radec(epoch="date")
            mra, mdec, mdist = ei.observe(moon).apparent().radec(epoch="date")
            samples.append((sra.radians, sdec.radians, sdist.km, mra.radians, mdec.radians, mdist.km))
        start = unix(tc) - SOLAR_HALF_S
        events.append((unix(tc), "ASTRO_SOLAR_ECLIPSE", len(tracks)))
        tracks.append((start, samples))
        print(f"solar eclipse {tc.utc_strftime('%Y-%m-%d %H:%M')} UTC: Oslo {oslo_cover(ts, eph, tc) * 100:.1f} %")

    events.sort()
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("/* Generated by scripts/build_astro.py (Skyfield, JPL DE440s) - do not edit.\n"
                f" * Astronomical events {first}-{last}, times in UTC. */\n\n"
                f"#define ASTRO_FIRST_YEAR {first}\n#define ASTRO_LAST_YEAR {last}\n"
                f"#define ASTRO_SOLAR_STEP_S {SOLAR_STEP_S}\n"
                f"#define ASTRO_SOLAR_SAMPLES {2 * SOLAR_HALF_S // SOLAR_STEP_S + 1}\n\n"
                "typedef struct {\n    uint32_t start; /* the first sample, UTC */\n"
                "    float s[ASTRO_SOLAR_SAMPLES][6];\n} astro_track_t;\n\n")
        f.write("static const char *const SHOWER_NAME[] = {\n")
        for _, name in SHOWERS:
            f.write(f'    "{name}",\n')
        f.write("};\n\nstatic const astro_event_t ASTRO_EVENTS[] = {\n")
        for t, kind, arg in events:
            f.write(f"    {{ {t}, {kind}, {arg} }},\n")
        f.write("};\n\n/* Sun RA, dec (rad, of date), distance (km); moon RA, dec, distance. */\n"
                "static const astro_track_t SOLAR_TRACKS[] = {\n")
        for start, samples in tracks:
            f.write(f"    {{ {start}, {{\n")
            for s in samples:
                f.write("        { " + ", ".join(c_float(v) for v in s) + " },\n")
            f.write("    } },\n")
        f.write("};\n")
    print(f"wrote {len(events)} events, {len(tracks)} solar eclipses to {os.path.relpath(OUT)}")


def c_float(v):
    s = f"{v:.9g}"
    return (s if any(ch in s for ch in ".e") else s + ".0") + "f"


def oslo_cover(ts, eph, tc):
    """The largest share of the sun's disc covered in Oslo (sun up)."""
    here = eph["earth"] + wgs84.latlon(*OSLO)
    best = 0.0
    for m in range(-240, 241):
        t = ts.tt_jd(tc.tt + m / 1440)
        o = here.at(t)
        s = o.observe(eph["sun"]).apparent()
        if s.altaz()[0].degrees < 0:
            continue
        mo = o.observe(eph["moon"]).apparent()
        d = s.separation_from(mo).radians
        rs = math.asin(696000 / s.distance().km)
        rm = math.asin(1737.4 / mo.distance().km)
        best = max(best, overlap(rs, rm, d))
    return best


def overlap(rs, rm, d):
    """Share of a disc of radius rs covered by one of radius rm, d apart."""
    if d >= rs + rm:
        return 0.0
    if d <= abs(rm - rs):
        return 1.0 if rm >= rs else (rm / rs) ** 2
    a = rs * rs * math.acos((d * d + rs * rs - rm * rm) / (2 * d * rs))
    b = rm * rm * math.acos((d * d + rm * rm - rs * rs) / (2 * d * rm))
    c = 0.5 * math.sqrt((-d + rs + rm) * (d + rs - rm) * (d - rs + rm) * (d + rs + rm))
    return (a + b - c) / (math.pi * rs * rs)


if __name__ == "__main__":
    main()
