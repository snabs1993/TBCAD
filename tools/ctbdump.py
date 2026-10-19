#!/usr/bin/env python3
"""Prints the decoded text of an AutoCAD .ctb / .stb plot style table (stdlib only).

Usage: python ctbdump.py table.ctb [--pens 1,2,7,251-255] > table.txt

Without --pens the whole text is printed. With --pens only the description, the
lineweight_table and the listed pens (palette colour numbers, 1-based) are printed.
"""
import re
import sys
import zlib


def decode(data):
    nl = data.find(b"pmzlibcodec")
    if nl < 0:
        raise SystemExit("not a compressed plot style table")
    z = data[nl + len(b"pmzlibcodec") + 12:]
    return zlib.decompress(z).decode("latin1")


def pens(spec):
    out = set()
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.update(range(int(a), int(b) + 1))
        elif part:
            out.add(int(part))
    return out


def main():
    text = decode(open(sys.argv[1], "rb").read())
    if "--pens" not in sys.argv:
        print(text)
        return
    want = pens(sys.argv[sys.argv.index("--pens") + 1])
    m = re.search(r'description="[^"]*"', text)
    if m:
        print(m.group(0))
    m = re.search(r"lineweight_table\{.*?\n\}", text, re.S)
    if m:
        print(m.group(0))
    styles = text[text.find("plot_style{"):]
    for m in re.finditer(r"^\s*(\d+)\{\s*\n(.*?)\n\s*\}", styles, re.S | re.M):
        if int(m.group(1)) + 1 in want:
            print(f" pen {int(m.group(1)) + 1}:")
            print(m.group(2).rstrip())


if __name__ == "__main__":
    main()
