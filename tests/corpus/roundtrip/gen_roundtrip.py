#!/usr/bin/env python3
"""Generates tests/corpus/roundtrip/*.dxf, the extra files of the DXF export round trip (W4-6).

The files exercise what the exporter must write back: every supported entity, hatch variants,
dimensions, blocks, linetypes, true colours, Thai text in R2000 and R2018, and the R12 form.
They come from ezdxf (MIT; used only here and in CI, never linked), so they are produced by a
writer that is independent of TBCAD's own.

    pip install ezdxf
    python3 tests/corpus/roundtrip/gen_roundtrip.py

The round trip test (libs/dxf/tests/writer/roundtrip_tests.cpp) reads every .dxf here and in
tests/corpus/reader, exports it and reads the export again; tests/corpus/roundtrip/audit_exports.py
checks the exports with ezdxf's audit.
"""
import math
import os

import ezdxf
from ezdxf.enums import MTextEntityAlignment, TextEntityAlignment

OUT = os.path.dirname(os.path.abspath(__file__))
THAI_HELLO = "สวัสดี"
THAI_ROOM = "ห้องนอน ชั้น 2"
THAI_WALL = "ผนัง"


def save(doc, name):
    doc.saveas(os.path.join(OUT, name), encoding="utf-8" if doc.dxfversion >= "AC1021" else None)


def new_doc(version):
    doc = ezdxf.new(version, setup=version != "R12", units=4 if version != "R12" else 0)
    return doc


def entities_r2000():
    doc = new_doc("R2000")
    doc.header["$INSUNITS"] = 4
    doc.linetypes.add("DASHED2", pattern=[7.5, 5.0, -2.5], description="Dashed __ __ __")
    doc.linetypes.add("DOTS", pattern=[2.0, 0.0, -2.0], description="Dots . . .")
    doc.layers.add("WALLS", color=1, linetype="DASHED2", lineweight=50)
    doc.layers.add("FURNITURE", color=210, lineweight=25)
    doc.layers.add("NOTES", color=3)
    doc.layers.add("HIDDEN", color=8, linetype="DOTS")
    doc.layers.get("HIDDEN").off()
    doc.layers.add("LOCKED", color=5).lock()
    doc.styles.add("ROMANT", font="romant.shx")
    doc.styles.add("WIDE", font="arial.ttf")
    doc.styles.get("WIDE").dxf.width = 1.2
    doc.styles.get("WIDE").dxf.oblique = 15

    blk = doc.blocks.new("CHAIR", base_point=(1, 1))
    blk.add_lwpolyline([(0, 0), (4, 0), (4, 4), (0, 4)], close=True, dxfattribs={"color": 0})
    blk.add_circle((2, 2), 1, dxfattribs={"color": 0})
    nested = doc.blocks.new("SET")
    nested.add_blockref("CHAIR", (0, 0))
    nested.add_blockref("CHAIR", (10, 0), dxfattribs={"rotation": 90, "xscale": 0.5, "yscale": 2})
    nested.add_line((0, -2), (10, -2))

    msp = doc.modelspace()
    msp.add_line((0, 0), (100, 0), dxfattribs={"layer": "WALLS"})
    msp.add_line((0, 0), (0, 80), dxfattribs={"layer": "WALLS", "linetype": "DOTS", "ltscale": 0.5})
    msp.add_circle((50, 40), 12.5, dxfattribs={"layer": "FURNITURE"})
    msp.add_arc((50, 40), 30, 20, 200, dxfattribs={"layer": "FURNITURE", "color": 4})
    msp.add_arc((50, 40), 35, 300, 40, dxfattribs={"true_color": ezdxf.colors.rgb2int((10, 200, 120))})
    pl = msp.add_lwpolyline(
        [(10, 10, 0, 0, 0.5), (30, 10, 1, 2, -0.3), (30, 30, 0, 0, 0), (10, 30)],
        format="xyseb",
        dxfattribs={"layer": "WALLS", "lineweight": 35},
    )
    pl.closed = True
    msp.add_lwpolyline([(60, 5), (70, 15), (80, 5)], dxfattribs={"const_width": 1.5})
    msp.add_point((5, 5))
    msp.add_ellipse((70, 60), major_axis=(15, 5), ratio=0.4)
    msp.add_ellipse((70, 60), major_axis=(15, 5), ratio=0.6, start_param=0.5, end_param=4.0)
    msp.add_spline(fit_points=[(0, 40), (10, 55), (20, 40), (30, 60)])
    msp.add_open_spline(control_points=[(40, 60), (45, 75), (55, 70), (60, 60), (70, 62)], degree=3)
    sp = msp.add_rational_spline(
        control_points=[(80, 60), (85, 75), (95, 70), (100, 60)], weights=[1, 2, 1, 1], degree=3
    )
    t = msp.add_text("Plain text", height=3, dxfattribs={"layer": "NOTES", "rotation": 30})
    t.set_placement((20, 70), align=TextEntityAlignment.LEFT)
    t = msp.add_text("Centered", height=2.5, dxfattribs={"style": "WIDE"})
    t.set_placement((50, 90), align=TextEntityAlignment.MIDDLE_CENTER)
    t = msp.add_text("Right", height=2.5)
    t.set_placement((100, 95), align=TextEntityAlignment.BOTTOM_RIGHT)
    t = msp.add_text("Fit text", height=2.5)
    t.set_placement((10, 95), p2=(40, 95), align=TextEntityAlignment.FIT)
    t = msp.add_text("Dia %%c100 %%d 45 %%p", height=2.5)
    t.set_placement((10, 100))
    m = msp.add_mtext("First line\\PSecond {\\fArial|b1|i0;bold} and {\\C1;red}\\P\\H2x;big\\H1;", dxfattribs={
        "char_height": 2.5, "width": 60, "layer": "NOTES"})
    m.set_location((10, 120), attachment_point=MTextEntityAlignment.TOP_LEFT)
    m = msp.add_mtext("Middle\\Pcentred", dxfattribs={"char_height": 3, "rotation": 45})
    m.set_location((150, 100), attachment_point=MTextEntityAlignment.MIDDLE_CENTER)
    msp.add_blockref("CHAIR", (20, 20), dxfattribs={"layer": "FURNITURE"})
    msp.add_blockref("SET", (60, 100), dxfattribs={"rotation": 15, "xscale": 1.5, "yscale": 1.5, "layer": "FURNITURE"})
    msp.add_line((0, 0), (50, 50), dxfattribs={"layer": "HIDDEN"})
    msp.add_line((10, 0), (60, 50), dxfattribs={"invisible": 1})
    msp.add_line((11, 0), (61, 50), dxfattribs={"layer": "LOCKED", "color": 256, "lineweight": 100})
    msp.add_line((12, 0), (62, 50), dxfattribs={"color": 140, "lineweight": 13})
    msp.add_leader([(100, 100), (110, 110), (120, 110)], dimstyle="Standard")
    save(doc, "rt_entities_r2000.dxf")


def hatches_r2000():
    doc = new_doc("R2000")
    msp = doc.modelspace()
    msp.add_lwpolyline([(0, 0), (100, 0), (100, 60), (0, 60)], close=True)
    msp.add_lwpolyline([(20, 15), (40, 15), (40, 45), (20, 45)], close=True)
    h = msp.add_hatch(color=2)
    h.paths.add_polyline_path([(0, 0), (100, 0), (100, 60), (0, 60)], is_closed=True, flags=1)
    h.paths.add_polyline_path([(20, 15), (40, 15), (40, 45), (20, 45)], is_closed=True, flags=0)
    h = msp.add_hatch(color=1)
    h.set_pattern_fill("ANSI31", scale=2, angle=30)
    h.paths.add_polyline_path([(120, 0), (200, 0), (200, 60), (120, 60)], is_closed=True)
    h = msp.add_hatch(color=3)
    h.set_pattern_fill("LINE45", scale=1.5)
    h.paths.add_polyline_path([(0, 80, 0), (50, 80, 0.5), (50, 120, 0), (0, 120, -0.4)], is_closed=True)
    h = msp.add_hatch(color=5)
    ep = h.paths.add_edge_path()
    ep.add_line((120, 80), (180, 80))
    ep.add_arc((180, 100), 20, -90, 90)
    ep.add_line((180, 120), (120, 120))
    ep.add_line((120, 120), (120, 80))
    h = msp.add_hatch(color=6)
    ep = h.paths.add_edge_path()
    ep.add_ellipse((250, 100), major_axis=(30, 0), ratio=0.5, start_angle=0, end_angle=360)
    h = msp.add_hatch(color=4)
    ep = h.paths.add_edge_path()
    ep.add_spline(
        control_points=[(0, 140), (20, 170), (60, 130), (80, 140)],
        knot_values=[0, 0, 0, 0, 1, 1, 1, 1],
        degree=3,
    )
    ep.add_line((80, 140), (0, 140))
    msp.add_solid([(300, 0), (310, 0), (300, 10), (310, 10)], dxfattribs={"color": 7})
    msp.add_solid([(300, 20), (310, 20), (305, 30)], dxfattribs={"color": 2})
    save(doc, "rt_hatch_r2000.dxf")


def thai_r2018():
    doc = new_doc("R2018")
    doc.layers.add(THAI_WALL, color=1)
    doc.layers.add("ห้อง~นอน", color=2)
    doc.styles.add("ไทย", font="tahoma.ttf")
    blk = doc.blocks.new("ประตู")
    blk.add_line((0, 0), (9, 0))
    blk.add_arc((0, 0), 9, 0, 90)
    msp = doc.modelspace()
    msp.add_line((0, 0), (100, 0), dxfattribs={"layer": THAI_WALL})
    t = msp.add_text(THAI_HELLO, height=5, dxfattribs={"style": "ไทย", "layer": THAI_WALL})
    t.set_placement((10, 10))
    m = msp.add_mtext(THAI_ROOM + "\\P" + THAI_HELLO, dxfattribs={"char_height": 4, "layer": "ห้อง~นอน"})
    m.set_location((10, 40))
    msp.add_blockref("ประตู", (50, 0))
    msp.add_text("Mixed ไทย + English 123", height=3).set_placement((10, 70))
    save(doc, "rt_thai_r2018.dxf")


def thai_r2000_cp874():
    """A Thai drawing written the way old Thai CAD programs do: R2000, code page 874."""
    doc = ezdxf.new("R2000", setup=True)
    doc.header["$DWGCODEPAGE"] = "ANSI_874"
    doc.layers.add(THAI_WALL, color=1)
    msp = doc.modelspace()
    msp.add_text(THAI_HELLO, height=5, dxfattribs={"layer": THAI_WALL}).set_placement((0, 0))
    msp.add_mtext(THAI_ROOM, dxfattribs={"char_height": 3}).set_location((0, 20))
    doc.saveas(os.path.join(OUT, "rt_thai_r2000_cp874.dxf"), encoding="cp874")


def dimensions_r2000():
    doc = new_doc("R2000")
    msp = doc.modelspace()
    msp.add_line((0, 0), (100, 0))
    msp.add_circle((50, 50), 20)
    msp.add_line((150, 0), (150, 80))
    msp.add_line((150, 0), (230, 0))
    msp.add_linear_dim(base=(0, -15), p1=(0, 0), p2=(100, 0)).render()
    msp.add_aligned_dim(p1=(0, 0), p2=(100, 30), distance=12).render()
    msp.add_radius_dim(center=(50, 50), mpoint=(63.0, 64.0), dimstyle="EZDXF").render()
    msp.add_diameter_dim(center=(50, 50), mpoint=(70, 50), dimstyle="EZDXF").render()
    msp.add_angular_dim_2l(base=(170, 30), line1=((150, 0), (150, 80)), line2=((150, 0), (230, 0))).render()
    msp.add_linear_dim(base=(10, 100), p1=(0, 80), p2=(60, 80), text="<> typ.", angle=0).render()
    save(doc, "rt_dimensions_r2000.dxf")


def r12_basic():
    doc = ezdxf.new("R12")
    doc.layers.add("A", color=2)
    blk = doc.blocks.new("BOX")
    blk.add_polyline2d([(0, 0), (5, 0), (5, 5), (0, 5)], close=True)
    msp = doc.modelspace()
    msp.add_line((0, 0), (10, 10), dxfattribs={"layer": "A"})
    msp.add_circle((5, 5), 2)
    msp.add_arc((5, 5), 4, 10, 100)
    pl = msp.add_polyline2d([(0, 0), (4, 0), (4, 4)], dxfattribs={"layer": "A"})
    pl.append_vertex((0, 4), dxfattribs={"bulge": 0.4})
    msp.add_text("R12 text", dxfattribs={"height": 2}).set_placement((1, 1))
    msp.add_blockref("BOX", (20, 20), dxfattribs={"rotation": 45})
    save(doc, "rt_r12.dxf")


def numbers_r2007():
    doc = new_doc("R2007")
    msp = doc.modelspace()
    msp.add_line((123456.789012345, -98765.4321098765), (1e-7, 3.14159265358979))
    msp.add_circle((1e6 + 0.123456789, 2e6), 0.000123456789)
    msp.add_line((1e9, 1e9), (1e9 + 0.001, 1e9 + 0.001))
    msp.add_arc((0, 0), 1e5, 0.000001, 359.999999)
    save(doc, "rt_numbers_r2007.dxf")


if __name__ == "__main__":
    entities_r2000()
    hatches_r2000()
    thai_r2018()
    thai_r2000_cp874()
    dimensions_r2000()
    r12_basic()
    numbers_r2007()
    print("written to", OUT)
