#!/usr/bin/env python3
"""Build the logos of the radio channels the Sonos screen offers (RADIOS in
components/sonos_parse.c) into server/bruk/radio/, from each channel's page
at myTuner (mytuner-radio.com). The update site serves them; the display
fetches them as it does covers (not built into the firmware, which is
copied into PSRAM whole at start-up: 200 KB of logos would cost that much
of it for good).

    python3 scripts/build_radio_logos.py

Needs Pillow. Each logo is the page's og:image, over white where it is
transparent, cut to its middle square, as baseline JPEGs:
    KEY.jpg    240 x 240: the Sonos screen's cover while it plays (and the
               Sonos app's, as the stream's album art)
    tiles.bin  every logo 104 x 104, the picker's tiles, in one file so they
               come with one request

tiles.bin's layout (little-endian; reader: sonos_logo_find in
components/sonos_parse.c):
    header  : char magic[4] = "RLG1", u32 count
    entries : count x 24 bytes: char key[16] (NUL padded), u32 offset, u32 len
              (offset from the start of the file)
    data    : the JPEGs, one after another
"""
import io
import os
import re
import struct
import sys
import urllib.request

from PIL import Image

# RADIOS' key -> its page at https://mytuner-radio.com/no/radio/<page>/
PAGES = {
    "nrk_p1": "nrk-p1-radio-401791",
    "nrk_p1pluss": "nrk-p1-420154",
    "nrk_p2": "nrk-p2-401799",
    "nrk_p3": "nrk-p3-radio-401800",
    "nrk_p3musikk": "nrk-p3-musikk-518204",
    "nrk_mp3": "nrk-mp3-radio-401782",
    "nrk_nyheter": "nrk-alltid-nyheter-401778",
    "nrk_klassisk": "nrk-klassisk-401781",
    "nrk_jazz": "nrk-jazz-401780",
    "nrk_folkemusikk": "nrk-folkemusikk-401779",
    "nrk_sport": "nrk-sport-401806",
    "nrk_super": "nrk-super-401807",
    "p4": "p4-402245",
    "radionorge": "radio-norge-408168",
    "p5": "p5-hits-402243",
    "p10": "p10-country-402237",
    "p7": "p7-klem-fm-402244",
    "p11": "p11-dance-501382",
    "nrj": "nrj-radio-norway-402246",
    "radiorock": "radio-rock-419008",
    "p9": "p9-retro-402239",
    "vinyl": "radio-vinyl-440318",
    "p8": "p8-pop-402240",
    "p6": "p6-rock-419850",
    "top40": "topp-40-446511",
    "p12": "p11-bandit-402236",  # P12 Hitmix, formerly P11 Bandit
    "p247mix": "p24-7mix-419277",
    "juleradioen": "juleradioen-449178",
}

COVER = 240  # main/sonos.c SO_COVER
TILE = 104   # main/music_tiles.h MT_TILE
UA = {"User-Agent": "Mozilla/5.0 (MultiDisplay logo build)"}


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=30) as r:
        return r.read()


def jpeg(im, side):
    out = io.BytesIO()
    im.resize((side, side), Image.LANCZOS).save(out, "JPEG", quality=85, optimize=True, progressive=False)
    return out.getvalue()


def logo(page):
    html = get(f"https://mytuner-radio.com/no/radio/{page}/").decode("utf-8", "replace")
    m = re.search(r'<meta property="og:image" content="([^"]+)"', html)
    if not m:
        sys.exit(f"{page}: no og:image")
    im = Image.open(io.BytesIO(get(m.group(1)))).convert("RGBA")
    bg = Image.new("RGBA", im.size, "white")
    bg.alpha_composite(im)
    im = bg.convert("RGB")
    w, h = im.size
    s = min(w, h)
    return im.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))


def main():
    folder = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "server", "bruk", "radio")
    os.makedirs(folder, exist_ok=True)
    tiles = []
    for key, page in PAGES.items():
        assert len(key) < 16, key
        im = logo(page)
        cover = jpeg(im, COVER)
        with open(os.path.join(folder, key + ".jpg"), "wb") as f:
            f.write(cover)
        tiles.append((key, jpeg(im, TILE)))
        print(f"{key:16} {len(cover):6} + {len(tiles[-1][1]):5} bytes")
    head = struct.pack("<4sI", b"RLG1", len(tiles))
    offset = len(head) + 24 * len(tiles)
    index = b""
    for key, data in tiles:
        index += struct.pack("<16sII", key.encode(), offset, len(data))
        offset += len(data)
    with open(os.path.join(folder, "tiles.bin"), "wb") as f:
        f.write(head + index + b"".join(d for _, d in tiles))
    print(f"{folder}: {len(tiles)} logos, tiles.bin {offset} bytes")


if __name__ == "__main__":
    main()
