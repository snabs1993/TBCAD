#!/usr/bin/env python3
"""Draws the TBCAD application icon and writes windows/tbcad.ico (PNG frames in an ICO).

The design is original: a dark rounded square, a drafting crosshair and a bold amber "T".
Run: python3 packaging/make_icon.py
"""
import struct
import sys
import zlib
from pathlib import Path

SIZES = (16, 24, 32, 48, 64, 128, 256)
BG = (27, 31, 38)
LINE = (120, 132, 150)
AMBER = (242, 169, 59)


def inside_round_rect(x, y, n, r):
    cx = min(max(x, r), n - 1 - r)
    cy = min(max(y, r), n - 1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def render(n, ss=4):
    """Returns RGBA bytes; supersampled by `ss` for smooth edges."""
    big = n * ss
    px = [[(0, 0, 0, 0)] * big for _ in range(big)]
    r = big * 0.18
    bar_t = big * 0.14                 # thickness of the T strokes
    bar_y0, bar_y1 = big * 0.24, big * 0.24 + bar_t
    bar_x0, bar_x1 = big * 0.22, big * 0.78
    stem_x0, stem_x1 = big * 0.5 - bar_t / 2, big * 0.5 + bar_t / 2
    stem_y1 = big * 0.80
    cross = max(1.0, big * 0.012)
    for y in range(big):
        for x in range(big):
            if not inside_round_rect(x, y, big, r):
                continue
            c = BG
            # crosshair through the middle of the stem, drawn behind the T
            if abs(y - big * 0.62) < cross or abs(x - big * 0.5) < cross:
                if big * 0.1 < x < big * 0.9 and big * 0.1 < y < big * 0.9:
                    c = LINE
            if (bar_x0 <= x <= bar_x1 and bar_y0 <= y <= bar_y1) or (
                stem_x0 <= x <= stem_x1 and bar_y0 <= y <= stem_y1
            ):
                c = AMBER
            px[y][x] = (*c, 255)
    out = bytearray()
    for y in range(n):
        for x in range(n):
            acc = [0, 0, 0, 0]
            for dy in range(ss):
                for dx in range(ss):
                    p = px[y * ss + dy][x * ss + dx]
                    a = p[3]
                    acc[0] += p[0] * a
                    acc[1] += p[1] * a
                    acc[2] += p[2] * a
                    acc[3] += a
            if acc[3] == 0:
                out += bytes((0, 0, 0, 0))
            else:
                out += bytes((acc[0] // acc[3], acc[1] // acc[3], acc[2] // acc[3], acc[3] // (ss * ss)))
    return bytes(out)


def png(n, rgba):
    raw = b"".join(b"\x00" + rgba[y * n * 4:(y + 1) * n * 4] for y in range(n))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    frames = [(n, png(n, render(n))) for n in SIZES]
    head = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    entries = b""
    for n, data in frames:
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out = Path(__file__).parent / "windows" / "tbcad.ico"
    out.write_bytes(head + entries + b"".join(d for _, d in frames))
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
