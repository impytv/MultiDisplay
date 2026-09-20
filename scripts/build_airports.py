#!/usr/bin/env python3
"""Build components/airports.bin, the airport + runway table the aircraft radar
draws, from the OurAirports open data (public domain,
https://ourairports.com/data/).

    python3 scripts/build_airports.py [airports.csv runways.csv]

Without arguments the two CSVs are downloaded. Which airports are kept:
  * every large and medium airport;
  * small airports with scheduled service;
  * other small airports that have a plain 4-letter ICAO-style identifier and a
    paved runway of at least 2000 ft (aerodromes like Kjeller, Eggemoen, ...).
Heliports, seaplane bases, closed fields and unnamed strips are left out.

File layout (little-endian; reader: main/main.c, radar_load_airports):
    header   : char magic[4] = "APT2", u32 airport_count, u32 runway_count
    airports : airport_count x 20 bytes
               char icao[4], char iata[4] (both space padded; iata is all
               spaces when the airport has none), i32 lat_e4, i32 lon_e4,
               u16 first_runway, u8 runway_count, u8 class (0 large, 1 medium, 2 small)
    runways  : runway_count x 16 bytes: i32 lat1_e4, lon1_e4, lat2_e4, lon2_e4
Coordinates are degrees x 10000 (about 11 m). Only runways that are open, have
both end coordinates and are at least 1500 ft long are kept.
"""
import collections
import csv
import io
import os
import re
import struct
import sys
import urllib.request

BASE = "https://davidmegginson.github.io/ourairports-data/"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components", "airports.bin")


def rows(arg_path, name):
    if arg_path:
        return list(csv.DictReader(open(arg_path, encoding="utf-8")))
    data = urllib.request.urlopen(BASE + name, timeout=120).read().decode("utf-8")
    return list(csv.DictReader(io.StringIO(data)))


def main():
    apts = rows(sys.argv[1] if len(sys.argv) > 2 else None, "airports.csv")
    rwys = rows(sys.argv[2] if len(sys.argv) > 2 else None, "runways.csv")

    by_ap = collections.defaultdict(list)
    for r in rwys:
        if r["closed"] == "1":
            continue
        if not (r["le_latitude_deg"] and r["le_longitude_deg"] and r["he_latitude_deg"] and r["he_longitude_deg"]):
            continue
        if int(float(r["length_ft"] or 0)) < 1500:
            continue
        by_ap[r["airport_ident"]].append(r)

    def paved(r):
        return any(x in (r["surface"] or "").upper() for x in ("ASP", "CON", "BIT", "PEM", "TAR", "PAV"))

    def keep(a):
        t = a["type"]
        if t in ("large_airport", "medium_airport"):
            return True
        if t != "small_airport":
            return False
        if a["scheduled_service"] == "yes":
            return True
        if not re.fullmatch(r"[A-Z]{4}", a["ident"]):
            return False
        return any(paved(r) and int(float(r["length_ft"] or 0)) >= 2000 for r in by_ap.get(a["ident"], []))

    def code(a):
        for c in (a["icao_code"], a["gps_code"], a["ident"]):
            if re.fullmatch(r"[A-Z0-9]{3,4}", c or ""):
                return c
        return a["ident"][:4]

    def iata(a):
        c = a["iata_code"] or ""
        return c if re.fullmatch(r"[A-Z0-9]{3}", c) else ""

    cls = {"large_airport": 0, "medium_airport": 1, "small_airport": 2}
    kept = [a for a in apts if keep(a)]
    kept.sort(key=lambda a: (float(a["latitude_deg"]), float(a["longitude_deg"])))

    ap_blob, rw_blob = bytearray(), bytearray()
    n_rw = 0
    for a in kept:
        rs = by_ap.get(a["ident"], [])[:8]
        ap_blob += struct.pack(
            "<4s4siiHBB",
            code(a).ljust(4).encode("ascii", "replace")[:4],
            iata(a).ljust(4).encode("ascii", "replace")[:4],
            round(float(a["latitude_deg"]) * 1e4),
            round(float(a["longitude_deg"]) * 1e4),
            n_rw, len(rs), cls[a["type"]],
        )
        for r in rs:
            rw_blob += struct.pack(
                "<iiii",
                round(float(r["le_latitude_deg"]) * 1e4), round(float(r["le_longitude_deg"]) * 1e4),
                round(float(r["he_latitude_deg"]) * 1e4), round(float(r["he_longitude_deg"]) * 1e4),
            )
        n_rw += len(rs)

    with open(OUT, "wb") as f:
        f.write(b"APT2" + struct.pack("<II", len(kept), n_rw) + ap_blob + rw_blob)
    print("%d airports, %d runways -> %s (%d bytes)" % (len(kept), n_rw, os.path.normpath(OUT), 12 + len(ap_blob) + len(rw_blob)))


if __name__ == "__main__":
    main()
