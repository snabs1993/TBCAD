#!/usr/bin/env python3
"""Writes the starter block library (assets/blocks/*.dxf): plan symbols in millimetres.

Each file is one symbol with its insertion point at the origin. INSERT takes the file name as the
block name. Run from the repository root: python3 tools/make_starter_blocks.py
"""
import math
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "blocks")


class Dxf:
    def __init__(self):
        self.ents = []

    def line(self, a, b):
        self.ents.append(["LINE", (10, a[0]), (20, a[1]), (11, b[0]), (21, b[1])])

    def circle(self, c, r):
        self.ents.append(["CIRCLE", (10, c[0]), (20, c[1]), (40, r)])

    def arc(self, c, r, a0, a1):
        self.ents.append(["ARC", (10, c[0]), (20, c[1]), (40, r), (50, a0), (51, a1)])

    def poly(self, pts, closed=True):
        e = ["LWPOLYLINE", (90, len(pts)), (70, 1 if closed else 0)]
        for p in pts:
            e += [(10, p[0]), (20, p[1])]
        self.ents.append(e)

    def rect(self, x0, y0, x1, y1):
        self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])

    def text(self):
        out = ["0", "SECTION", "2", "HEADER", "9", "$ACADVER", "1", "AC1015", "9", "$INSUNITS",
               "70", "4", "0", "ENDSEC", "0", "SECTION", "2", "ENTITIES"]
        for e in self.ents:
            out += ["0", e[0], "8", "0"]
            for code, val in e[1:]:
                out += [str(code), ("%.6g" % val) if isinstance(val, float) else str(val)]
        out += ["0", "ENDSEC", "0", "EOF"]
        return "\n".join(out) + "\n"

    def save(self, name):
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, name + ".dxf"), "w", newline="\n") as f:
            f.write(self.text())


def door(w):
    d = Dxf()
    d.rect(0, 0, 40, w)  # leaf, open at 90 degrees, hinge at the origin
    d.arc((0, 0), w, 0, 90)  # swing
    d.save("Door-%d" % w)


def door_double(w):
    d = Dxf()
    half = w / 2
    d.rect(0, 0, 40, half)
    d.rect(w - 40, 0, w, half)
    d.arc((0, 0), half, 0, 90)
    d.arc((w, 0), half, 90, 180)
    d.save("Door-Double-%d" % w)


def door_sliding(w):
    d = Dxf()
    half = w / 2
    d.rect(0, 0, half + 50, 40)
    d.rect(half - 50, 60, w, 100)
    d.save("Door-Sliding-%d" % w)


def window(w):
    d = Dxf()
    d.rect(0, 0, w, 200)  # the wall opening, 200 thick
    d.line((0, 70), (w, 70))
    d.line((0, 130), (w, 130))
    d.save("Window-%d" % w)


def column_square(s):
    d = Dxf()
    d.rect(-s / 2, -s / 2, s / 2, s / 2)
    d.line((-s / 2, -s / 2), (s / 2, s / 2))
    d.line((-s / 2, s / 2), (s / 2, -s / 2))
    d.save("Column-%d" % s)


def column_round(dia):
    d = Dxf()
    d.circle((0, 0), dia / 2)
    d.line((-dia / 2, 0), (dia / 2, 0))
    d.line((0, -dia / 2), (0, dia / 2))
    d.save("Column-Round-%d" % dia)


def north_arrow():
    d = Dxf()
    d.circle((0, 0), 250)
    d.poly([(0, 250), (-90, -150), (0, -60), (90, -150)])
    d.save("North-Arrow")


def bed_double():
    d = Dxf()
    d.rect(0, 0, 1600, 2000)
    d.rect(100, 1650, 750, 1900)
    d.rect(850, 1650, 1500, 1900)
    d.line((0, 1400), (1600, 1400))
    d.save("Bed-Double")


def table(w, h):
    d = Dxf()
    d.rect(0, 0, w, h)
    d.save("Table-%dx%d" % (w, h))


def chair():
    d = Dxf()
    d.rect(0, 0, 450, 450)
    d.rect(0, 400, 450, 450)
    d.save("Chair")


def sofa3():
    d = Dxf()
    d.rect(0, 0, 2000, 900)
    d.rect(100, 100, 1900, 600)
    d.rect(0, 0, 150, 900)
    d.rect(1850, 0, 2000, 900)
    d.save("Sofa-3-Seat")


def toilet():
    d = Dxf()
    d.rect(0, 0, 400, 200)  # tank
    n = 24
    pts = [(200 + 175 * math.cos(2 * math.pi * i / n) * 1.0,
            200 + 250 + 250 * math.sin(2 * math.pi * i / n)) for i in range(n)]
    d.poly(pts)
    d.save("Toilet")


def basin():
    d = Dxf()
    d.rect(0, 0, 550, 450)
    d.circle((275, 225), 150)
    d.circle((275, 225), 20)
    d.save("Basin")


for w in (700, 800, 900, 1000):
    door(w)
door_double(1600)
door_double(1800)
door_sliding(1800)
door_sliding(2400)
for w in (600, 900, 1200, 1500, 1800):
    window(w)
column_square(300)
column_square(400)
column_round(300)
column_round(400)
north_arrow()
bed_double()
table(1200, 800)
table(1800, 900)
chair()
sofa3()
toilet()
basin()
