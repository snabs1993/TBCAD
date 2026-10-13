#!/usr/bin/env python3
"""Writes a large synthetic DXF (lines, circles, polylines, text on 20 layers) for timing import.

usage: gen_big_dxf.py OUT.dxf [megabytes]   (default 100)
"""
import random
import sys

out = sys.argv[1]
target = int(sys.argv[2] if len(sys.argv) > 2 else 100) * 1_000_000
random.seed(1)
with open(out, "w") as o:
    def w(c, v):
        o.write(f"{c}\n{v}\n")

    w(0, "SECTION"); w(2, "HEADER"); w(9, "$ACADVER"); w(1, "AC1015"); w(0, "ENDSEC")
    w(0, "SECTION"); w(2, "TABLES"); w(0, "TABLE"); w(2, "LAYER")
    for i in range(20):
        w(0, "LAYER"); w(2, f"L{i}"); w(70, 0); w(62, i + 1); w(6, "CONTINUOUS")
    w(0, "ENDTAB"); w(0, "ENDSEC")
    w(0, "SECTION"); w(2, "ENTITIES")
    n = 0
    while o.tell() < target:
        k = n % 4
        layer = f"L{n % 20}"
        x = random.uniform(0, 1e5)
        y = random.uniform(0, 1e5)
        if k == 0:
            w(0, "LINE"); w(8, layer); w(10, x); w(20, y); w(30, 0)
            w(11, x + random.uniform(-50, 50)); w(21, y + random.uniform(-50, 50)); w(31, 0)
        elif k == 1:
            w(0, "CIRCLE"); w(8, layer); w(10, x); w(20, y); w(30, 0); w(40, random.uniform(1, 20))
        elif k == 2:
            w(0, "LWPOLYLINE"); w(8, layer); w(90, 6); w(70, 0)
            for j in range(6):
                w(10, x + j * 5); w(20, y + random.uniform(0, 9))
        else:
            w(0, "TEXT"); w(8, layer); w(10, x); w(20, y); w(30, 0); w(40, 2.5); w(1, f"Label {n}")
        n += 1
    w(0, "ENDSEC"); w(0, "EOF")
print(n, "entities")
