#!/usr/bin/env python3
"""Writes a small colour-dependent plot style table (.ctb) for the scenario tests.

Laid out like a typical office "EP" table: most pens plot black at a fixed width, while the
greys (ACI 251-255, 240, 230, 193) keep the object colour (colour policy 0), as office tables
do for hatch fills. Usage: make_test_ctb.py OUT.ctb
"""
import struct
import sys
import zlib

BLACK = -1023410176  # 0xC3000000: a fixed RGB colour, black
OBJECT = -1006632961  # placeholder white; colour policy 0 keeps the object colour
KEEP = {251, 252, 253, 254, 255, 240, 230, 193}
WEIGHTS = [0.0, 0.05, 0.09, 0.13, 0.15, 0.18, 0.20, 0.25, 0.30, 0.35, 0.40, 0.50, 0.55, 0.60]


def text():
    out = ['description="Scenario test table (EP style)"', "aci_table_available=TRUE",
           "scale_factor=1.0", "apply_factor=FALSE", "custom_lineweight_table{"]
    out += [f" {i}={w}" for i, w in enumerate(WEIGHTS)]
    out += ["}", "plot_style{"]
    for k in range(255):
        aci = k + 1
        keep = aci in KEEP
        out += [f" {k}{{", f'  name="Color_{aci}"',
                f"  color={OBJECT if keep else BLACK}",
                f"  color_policy={0 if keep else 4}",
                "  screen=100",
                # pen 7 (white/black lines) 0.25 mm, others 0.18 mm, greys keep the object's
                f"  lineweight={255 if keep else (7 if aci == 7 else 5)}",
                " }"]
    out += ["}"]
    return ("\n".join(out) + "\n").encode("ascii")


def main():
    raw = text()
    z = zlib.compress(raw)
    header = b"PIAFILEVERSION_2.0,CTBVER1,compress\r\npmzlibcodec"
    with open(sys.argv[1], "wb") as f:
        f.write(header + struct.pack("<III", zlib.adler32(z), len(raw), len(z)) + z)


if __name__ == "__main__":
    main()
