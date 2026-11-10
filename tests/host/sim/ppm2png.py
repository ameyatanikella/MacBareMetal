#!/usr/bin/env python3
"""Convert the simulator's PPM snapshots to PNG (standard library only).

usage: ppm2png.py <dir> [scale-divisor]
"""
import struct
import sys
import zlib
from pathlib import Path


def read_ppm(path):
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6" and parts[2] == b"255", "unexpected PPM header"
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def write_png(path, w, h, rgb):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def downscale(w, h, rgb, d):
    if d <= 1:
        return w, h, rgb
    nw, nh = w // d, h // d
    out = bytearray(nw * nh * 3)
    for y in range(nh):
        src = y * d * w * 3
        row = rgb[src:src + w * 3]
        for x in range(nw):
            out[(y * nw + x) * 3:(y * nw + x) * 3 + 3] = row[x * d * 3:x * d * 3 + 3]
    return nw, nh, bytes(out)


def main():
    folder = Path(sys.argv[1])
    div = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    for ppm in sorted(folder.glob("*.ppm")):
        w, h, rgb = downscale(*read_ppm(ppm), div)
        write_png(ppm.with_suffix(".png"), w, h, rgb)
        ppm.unlink()
        print(ppm.with_suffix(".png"))


if __name__ == "__main__":
    main()
