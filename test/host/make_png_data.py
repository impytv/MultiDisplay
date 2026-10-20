#!/usr/bin/env python3
"""The PNG decoder's test pictures (data/png/) and the squares it should
make of them (data/png/expected.h), for test_png.c. Written here byte by
byte, so every colour type, bit depth and row filter is covered and the
expected pixels don't depend on another decoder. Run from test/host:

    python3 make_png_data.py
"""

import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "png")
BG = (255, 255, 255)
CHANS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def pack_row(samples, depth):
    if depth == 16:
        return b"".join(struct.pack(">H", v) for v in samples)
    if depth == 8:
        return bytes(samples)
    out, acc, bits = bytearray(), 0, 0
    for v in samples:
        acc = (acc << depth) | v
        bits += depth
        if bits == 8:
            out.append(acc)
            acc, bits = 0, 0
    if bits:
        out.append(acc << (8 - bits))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filt(f, row, prev, bpp):
    out = bytearray()
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
        out.append((x - pred) & 0xFF)
    return bytes([f]) + bytes(out)


def write_png(name, w, h, ctype, depth, rows, plte=None, trns=None, interlace=0, idat_size=None, cut=0):
    bpp = max(1, CHANS[ctype] * depth // 8)
    raw, prev = b"", None
    for y, samples in enumerate(rows):
        row = pack_row(samples, depth)
        prev = prev or bytes(len(row))
        raw += filt(y % 5, row, prev, bpp)  # each filter in turn
        prev = row
    z = zlib.compress(raw, 9)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))
    if plte:
        data += chunk(b"PLTE", b"".join(bytes(c) for c in plte))
    if trns is not None:
        data += chunk(b"tRNS", trns)
    step = idat_size or len(z)
    for i in range(0, len(z), step):
        data += chunk(b"IDAT", z[i : i + step])
    data += chunk(b"IEND", b"")
    if cut:
        data = data[:-cut]
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)


def to8(v, depth):
    return v >> 8 if depth == 16 else v if depth == 8 else v * 255 // ((1 << depth) - 1)


def over(rgb, a):
    return tuple((c * a + bg * (255 - a) + 127) // 255 if a < 255 else c for c, bg in zip(rgb, BG))


def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)


def square(pixels, w, h, side):
    """What png_decode_square makes of pixels[y][x] (r, g, b, over BG)."""
    s = min(w, h)
    ox, oy = (w - s) // 2, (h - s) // 2
    out = []
    if s >= side:
        acc = [[[0, 0, 0, 0] for _ in range(side)] for _ in range(side)]
        for y in range(s):
            for x in range(s):
                a = acc[y * side // s][x * side // s]
                p = pixels[oy + y][ox + x]
                for c in range(3):
                    a[c] += p[c]
                a[3] += 1
        for row in acc:
            for r, g, b, n in row:
                out.append(rgb565((r + n // 2) // n, (g + n // 2) // n, (b + n // 2) // n))
    else:
        for ty in range(side):
            for tx in range(side):
                out.append(rgb565(*pixels[oy + ty * s // side][ox + tx * s // side]))
    return out


def lcg(seed):
    while True:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        yield seed >> 8


EXPECTED = []


def case(name, w, h, ctype, depth, side, gen, plte=None, trns=None, key=None, **kw):
    """gen(x, y) gives the samples of a pixel; key is tRNS's colour."""
    rows = [[v for x in range(w) for v in gen(x, y)] for y in range(h)]
    pal_a = list(trns) if ctype == 3 and trns else []
    pixels = []
    for y in range(h):
        line = []
        for x in range(w):
            v = gen(x, y)
            if ctype == 0:
                rgb, a = (to8(v[0], depth),) * 3, 0 if key is not None and v[0] == key[0] else 255
            elif ctype == 2:
                rgb, a = tuple(to8(c, depth) for c in v), 0 if key is not None and tuple(v) == tuple(key) else 255
            elif ctype == 3:
                rgb, a = plte[v[0]], pal_a[v[0]] if v[0] < len(pal_a) else 255
            elif ctype == 4:
                rgb, a = (to8(v[0], depth),) * 3, to8(v[1], depth)
            else:
                rgb, a = tuple(to8(c, depth) for c in v[:3]), to8(v[3], depth)
            line.append(over(rgb, a))
        pixels.append(line)
    write_png(name + ".png", w, h, ctype, depth, rows, plte=plte, trns=trns, **kw)
    EXPECTED.append((name, side, square(pixels, w, h, side)))


def main():
    os.makedirs(OUT, exist_ok=True)
    r = lcg(7)
    noise = [[next(r) for _ in range(64)] for _ in range(64)]

    # RGBA, 8 bits, wider than tall (the middle cropped), shrunk by 3.
    case("rgba8", 40, 24, 6, 8, 8,
         lambda x, y: (noise[y][x] & 255, (x * 6) & 255, (y * 10) & 255, (noise[x][y] >> 3) & 255))
    # RGB, 16 bits, shrunk by 2.5 - and in IDAT chunks of 7 bytes.
    case("rgb16", 20, 20, 2, 16, 8,
         lambda x, y: ((x * 3000 + noise[y][x]) & 0xFFFF, (y * 3100) & 0xFFFF, (noise[x][y] * 7) & 0xFFFF),
         idat_size=7)
    # A 4-bit palette with transparent entries (tRNS), grown 13 -> 26.
    plte = [((i * 37) & 255, (i * 91) & 255, (255 - i * 13) & 255) for i in range(16)]
    case("pal4", 13, 13, 3, 4, 26, lambda x, y: ((x + 2 * y + noise[y][x]) % 16,), plte=plte,
         trns=bytes([0, 128, 255, 40]))
    # Grey, 1 bit, taller than wide.
    case("grey1", 9, 17, 0, 1, 8, lambda x, y: ((noise[y][x] >> 4) & 1,))
    # Grey with alpha, 8 bits.
    case("ga8", 10, 10, 4, 8, 8, lambda x, y: (noise[y][x] & 255, (x * 25) & 255))
    # Grey, 16 bits, with a transparent grey (tRNS).
    case("grey16", 12, 12, 0, 16, 8, lambda x, y: (1000 if (x + y) % 3 == 0 else (x * 5000 + y * 7) & 0xFFFF,),
         trns=struct.pack(">H", 1000), key=(1000,))
    # RGB, 8 bits, a transparent colour (tRNS), shrunk to the same size.
    case("rgb8", 8, 8, 2, 8, 8, lambda x, y: (10, 20, 30) if x == y else (x * 30, y * 30, noise[y][x] & 255),
         trns=struct.pack(">HHH", 10, 20, 30), key=(10, 20, 30))
    # Grey, 2 bits, grown 6 -> 8 (not a whole factor).
    case("grey2", 6, 6, 0, 2, 8, lambda x, y: ((x + y) % 4,))

    # What it can't read: interlaced, and cut short.
    write_png("interlaced.png", 4, 4, 0, 8, [[1, 2, 3, 4]] * 4, interlace=1)
    write_png("cut.png", 30, 30, 2, 8, [[v for x in range(30) for v in (x, y, noise[y][x] & 255)] for y in range(30)],
              cut=40)

    # One as an image library writes it (its own filters, several IDATs).
    try:
        from PIL import Image
    except ImportError:
        Image = None
    if Image is not None:
        w = h = 50
        im = Image.new("RGBA", (w, h))
        im.putdata([((x * 5) & 255, (y * 5) & 255, noise[y][x] & 255, 255 if (x // 10 + y // 10) % 2 else 90)
                    for y in range(h) for x in range(w)])
        im.save(os.path.join(OUT, "pil.png"), optimize=True)
        pixels = [[over(im.getpixel((x, y))[:3], im.getpixel((x, y))[3]) for x in range(w)] for y in range(h)]
        EXPECTED.append(("pil", 24, square(pixels, w, h, 24)))

    with open(os.path.join(OUT, "expected.h"), "w") as f:
        f.write("/* Made by make_png_data.py: what png_decode_square should make of data/png/. */\n\n")
        f.write("typedef struct {\n    const char *file;\n    int side;\n    const uint16_t *px;\n} png_case_t;\n\n")
        for name, side, px in EXPECTED:
            f.write(f"static const uint16_t PX_{name.upper()}[{len(px)}] = {{\n")
            for i in range(0, len(px), 12):
                f.write("    " + ", ".join(f"0x{v:04x}" for v in px[i : i + 12]) + ",\n")
            f.write("};\n\n")
        f.write("static const png_case_t PNG_CASES[] = {\n")
        for name, side, _ in EXPECTED:
            f.write(f'    {{ "data/png/{name}.png", {side}, PX_{name.upper()} }},\n')
        f.write("};\n")


if __name__ == "__main__":
    main()
