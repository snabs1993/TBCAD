#!/usr/bin/env python3
"""Generates tests/corpus/reader/*.dxf and their .expected.json files.

Everything here is made by the TBCAD project: files come from ezdxf (MIT, used only in this
script and in CI, never linked) or are written by hand below. No third-party drawings.

    pip install ezdxf
    python3 tests/corpus/gen/gen_corpus.py            # rewrites tests/corpus/reader
    python3 tests/corpus/gen/check_corpus_ezdxf.py    # cross-check with ezdxf

The expected files are written from what the generator put into each drawing, never from the
output of the TBCAD reader. Keys of an .expected.json (all optional except "exit"):

    description  text
    options      {"splitLayers": "~", "insunits": 1, "encoding": "cp874"} passed to the reader
    exit         0 clean, 1 with warnings, 2 failed (the CLI exit code)
    version      $ACADVER found in the file
    encoding     name of the encoding the reader must pick (ANSI_874, UTF-8, ...)
    binary       true for binary DXF
    unitScale    millimetres per drawing unit
    dxf          {"LINE": 3, ...}  records read per DXF type (model space + block definitions)
    kinds        {"Line": 3, ...}  model space entities of the document by kind
    layers       sorted layer paths ("A/edge" for a sub-layer)
    texts        sorted plain strings of all Text and MText entities
    warnings     sorted set of diagnostic codes with severity warning or error
    ezdxf        false when ezdxf is not expected to read the file (raw or broken files)
"""
import json
import math
import os
import struct
import sys

import ezdxf
from ezdxf.enums import TextEntityAlignment

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reader")
THAI_HELLO = "สวัสดี"
THAI_BEDROOM = "ห้องนอน"
THAI_BATH = "ห้องน้ำ"
THAI_WALL = "ผนัง"


def write(name, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    kwargs = {} if mode == "wb" else {"encoding": "utf-8", "newline": "\n"}
    with open(os.path.join(OUT, name), mode, **kwargs) as f:
        f.write(data)


def expect(name, **fields):
    fields = {k: v for k, v in fields.items() if v is not None}
    with open(os.path.join(OUT, name + ".expected.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(fields, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")


def new_doc(version, **kwargs):
    doc = ezdxf.new(version, **kwargs)
    if version != "R12":
        doc.header["$INSUNITS"] = 4  # millimetres
    return doc


def save_ezdxf(doc, name, **expected):
    if "layers" in expected:
        expected["layers"] = sorted(set(expected["layers"]) | {"Defpoints"})  # ezdxf always adds it
    doc.saveas(os.path.join(OUT, name + ".dxf"))
    expect(name, **expected)


# ---------------------------------------------------------------------------------------------
# A tiny DXF writer for the files that ezdxf would not produce (raw, broken, binary).

def kind(code):
    if code <= 4 or 6 <= code <= 9 or 100 == code or code == 102 or 300 <= code <= 309:
        return "S"
    if code == 5 or 320 <= code <= 369 or 390 <= code <= 399:
        return "H"
    if 10 <= code <= 59 or 110 <= code <= 149 or 210 <= code <= 239 or 460 <= code <= 469:
        return "D"
    if 60 <= code <= 79 or 170 <= code <= 179 or 270 <= code <= 289 or 370 <= code <= 389:
        return "I2"
    if 90 <= code <= 99 or 420 <= code <= 429 or 440 <= code <= 459:
        return "I4"
    if 290 <= code <= 299:
        return "B"
    if 310 <= code <= 319:
        return "X"
    return "S"


def ascii_dxf(pairs, eol="\n"):
    out = []
    for code, value in pairs:
        if kind(code) == "D":
            value = repr(float(value))
        out.append(f"{code:>3}{eol}{value}{eol}")
    return "".join(out)


def binary_dxf(pairs, width=2):
    out = b"AutoCAD Binary DXF\r\n\x1a\x00"
    for code, value in pairs:
        out += struct.pack("<h", code) if width == 2 else bytes([code])
        k = kind(code)
        if k in ("S", "H"):
            out += str(value).encode("utf-8") + b"\x00"
        elif k == "D":
            out += struct.pack("<d", float(value))
        elif k == "I2":
            out += struct.pack("<h", int(value))
        elif k == "I4":
            out += struct.pack("<i", int(value))
        elif k == "B":
            out += bytes([1 if value else 0])
        elif k == "X":
            out += bytes([len(value)]) + value
    return out


def section(name, pairs):
    return [(0, "SECTION"), (2, name)] + pairs + [(0, "ENDSEC")]


def header(version="AC1015", insunits=None, extra=None):
    p = [(9, "$ACADVER"), (1, version)]
    if insunits is not None:
        p += [(9, "$INSUNITS"), (70, insunits)]
    return section("HEADER", p + (extra or []))


def layer_table(*layers):
    rows = []
    for name, color in layers:
        rows += [(0, "LAYER"), (2, name), (70, 0), (62, color), (6, "CONTINUOUS")]
    return section("TABLES", [(0, "TABLE"), (2, "LAYER"), (70, len(layers))] + rows + [(0, "ENDTAB")])


def line(layer, x1, y1, x2, y2):
    return [(0, "LINE"), (8, layer), (10, x1), (20, y1), (30, 0.0), (11, x2), (21, y2), (31, 0.0)]


def eof():
    return [(0, "EOF")]


# ---------------------------------------------------------------------------------------------

def gen_r12():
    doc = new_doc("R12")
    doc.layers.add("WALLS", color=1)
    msp = doc.modelspace()
    for i in range(3):
        msp.add_line((0, i), (10, i), dxfattribs={"layer": "WALLS"})
    msp.add_circle((5, 5), 2)
    msp.add_arc((0, 0), 4, 0, 90)
    msp.add_text("Hello R12", dxfattribs={"height": 2.5, "insert": (1, 1)})
    msp.add_polyline2d([(0, 0), (10, 0), (10, 10)], close=True)
    msp.add_point((3, 3))
    blk = doc.blocks.new("BOX")
    for a, b in [((0, 0), (1, 0)), ((1, 0), (1, 1)), ((1, 1), (0, 1)), ((0, 1), (0, 0))]:
        blk.add_line(a, b)
    msp.add_blockref("BOX", (20, 20))
    save_ezdxf(doc, "r12_basic", description="R12 with the basic entities and a block", exit=0,
               version="AC1009", encoding="ANSI_1252", unitScale=1.0,
               dxf={"LINE": 7, "CIRCLE": 1, "ARC": 1, "TEXT": 1, "POLYLINE": 1, "POINT": 1, "INSERT": 1},
               kinds={"Line": 3, "Circle": 1, "Arc": 1, "Text": 1, "Polyline": 1, "Point": 1, "Insert": 1},
               layers=["0", "WALLS"], texts=["Hello R12"], warnings=[])


def gen_r2000_entities():
    doc = new_doc("R2000")
    doc.layers.add("A", color=3)
    doc.layers.add("B", true_color=0x336699)
    msp = doc.modelspace()
    msp.add_line((0, 0), (10, 5), dxfattribs={"layer": "A"})
    msp.add_line((0, 5), (10, 0), dxfattribs={"layer": "B", "color": 1})
    msp.add_circle((5, 5), 2.5)
    msp.add_arc((0, 0), 6, 30, 120)
    msp.add_lwpolyline([(0, 0, 0, 0, 0.5), (4, 0, 0, 0, 0), (4, 3)], format="xyseb", close=True)
    msp.add_lwpolyline([(0, 10), (5, 10), (5, 15)], dxfattribs={"const_width": 0.5})
    msp.add_polyline2d([(20, 0), (25, 0), (25, 5)])
    msp.add_ellipse((30, 0), major_axis=(5, 0), ratio=0.5)
    msp.add_ellipse((30, 10), major_axis=(0, 4), ratio=0.25, start_param=0.5, end_param=2.5)
    msp.add_spline(fit_points=[(0, 20), (2, 23), (5, 21), (8, 25)])
    msp.add_open_spline(control_points=[(0, 30), (2, 33), (5, 31), (8, 35)], degree=3)
    t = msp.add_text("Plain text", dxfattribs={"height": 2.0, "rotation": 15})
    t.set_placement((1, 1))
    t2 = msp.add_text("Centered", dxfattribs={"height": 3.0})
    t2.set_placement((40, 40), align=TextEntityAlignment.MIDDLE_CENTER)
    msp.add_mtext("First\\PSecond line", dxfattribs={"insert": (2, 2), "char_height": 2, "width": 30})
    msp.add_point((7, 7))
    blk = doc.blocks.new("MARK")
    blk.add_line((-1, 0), (1, 0))
    blk.add_line((0, -1), (0, 1))
    msp.add_blockref("MARK", (50, 50), dxfattribs={"xscale": 2, "yscale": 2, "rotation": 45})
    h1 = msp.add_hatch(color=2)
    h1.paths.add_polyline_path([(0, 40), (10, 40), (10, 50), (0, 50)], is_closed=True)
    h2 = msp.add_hatch(color=5)
    h2.set_pattern_fill("ANSI31", scale=0.5, angle=45)
    h2.paths.add_polyline_path([(20, 40), (30, 40), (30, 50), (20, 50)], is_closed=True)
    h2.paths.add_polyline_path([(23, 43), (27, 43), (27, 47), (23, 47)], is_closed=True)
    h3 = msp.add_hatch(color=3)
    e = h3.paths.add_edge_path()
    e.add_line((40, 40), (50, 40))
    e.add_arc((50, 45), 5, 270, 90, ccw=True)
    e.add_line((50, 50), (40, 50))
    e.add_arc((40, 45), 5, 90, 270, ccw=True)
    msp.add_solid([(60, 0), (64, 0), (60, 3), (64, 3)])
    msp.add_trace([(60, 10), (64, 10), (60, 13), (64, 13)])
    msp.add_leader([(70, 0), (75, 5), (80, 5)])
    save_ezdxf(doc, "r2000_entities", description="R2000 with every mapped entity type", exit=0,
               version="AC1015", encoding="ANSI_1252", unitScale=1.0,
               dxf={"LINE": 4, "CIRCLE": 1, "ARC": 1, "LWPOLYLINE": 2, "POLYLINE": 1, "ELLIPSE": 2,
                    "SPLINE": 2, "TEXT": 2, "MTEXT": 1, "POINT": 1, "INSERT": 1, "HATCH": 3,
                    "SOLID": 1, "TRACE": 1, "LEADER": 1},
               kinds={"Line": 2, "Circle": 1, "Arc": 1, "Polyline": 3, "Ellipse": 2, "Spline": 2,
                      "Text": 2, "MText": 1, "Point": 1, "Insert": 1, "Hatch": 5, "Leader": 1},
               layers=["0", "A", "B"], texts=["Centered", "First\nSecond line", "Plain text"],
               warnings=[])


def gen_dimensions():
    doc = new_doc("R2000", setup=True)
    msp = doc.modelspace()
    msp.add_linear_dim(base=(0, -3), p1=(0, 0), p2=(10, 0)).render()
    msp.add_aligned_dim(p1=(0, 5), p2=(8, 11), distance=2).render()
    msp.add_circle((30, 5), 4)
    msp.add_radius_dim(center=(30, 5), mpoint=(33, 8), dimstyle="EZ_RADIUS").render()
    msp.add_circle((50, 5), 4)
    msp.add_diameter_dim(center=(50, 5), mpoint=(53, 8), dimstyle="EZ_RADIUS").render()
    msp.add_angular_dim_2l(base=(70, 20), line1=((70, 0), (80, 0)), line2=((70, 0), (75, 10))).render()
    msp.add_angular_dim_3p(base=(100, 20), center=(100, 0), p1=(110, 0), p2=(105, 10)).render()
    doc.saveas(os.path.join(OUT, "r2000_dimensions.dxf"))
    # The expected numbers are filled in by hand after looking at what ezdxf rendered into the
    # *D blocks: texts and graphics are checked loosely (see the C++ test).
    expect("r2000_dimensions", description="R2000 with six dimensions and their *D blocks", exit=0,
           version="AC1015", encoding="ANSI_1252",
           dxf_min={"DIMENSION": 6}, kinds_min={"Dimension": 6, "Circle": 2}, warnings=[])


def gen_layers():
    doc = new_doc("R2004")
    doc.linetypes.add("DASHED", pattern=[0.5, 0.25, -0.25], description="Dashed")
    names = {}
    msp = doc.modelspace()

    def add(name, **kw):
        lt = kw.pop("linetype", None)
        lay = doc.layers.add(name, **kw)
        if lt:
            lay.dxf.linetype = lt
        msp.add_line((0, len(names)), (10, len(names)), dxfattribs={"layer": name})
        names[name] = True
        return lay

    add("WALLS", color=1, lineweight=50, linetype="DASHED")
    add("HIDDEN", color=2).off()
    add("FROZEN", color=3).freeze()
    add("LOCKED", color=4).lock()
    add("NOPLOT", color=5).dxf.plot = 0
    add("TRUE", true_color=0xFF8000)
    save_ezdxf(doc, "r2004_layers", description="R2004 layer table: colours, lineweight, on/frozen/locked/plot",
               exit=0, version="AC1018", encoding="ANSI_1252",
               dxf={"LINE": 6}, kinds={"Line": 6},
               layers=["0", "FROZEN", "HIDDEN", "LOCKED", "NOPLOT", "TRUE", "WALLS"],
               warnings=[])


def gen_tilde_layers():
    doc = new_doc("R2004")
    msp = doc.modelspace()
    layers = ["S-FLOOR", "S-FLOOR~edge", "S-FLOOR~Floor hidden", "X~y~z"]
    for i, n in enumerate(layers):
        doc.layers.add(n, color=i + 1)
        msp.add_line((0, i), (10, i), dxfattribs={"layer": n})
    doc.saveas(os.path.join(OUT, "layers_tilde.dxf"))
    doc.saveas(os.path.join(OUT, "layers_tilde_split.dxf"))
    common = dict(exit=0, version="AC1018", encoding="ANSI_1252", dxf={"LINE": 4}, kinds={"Line": 4},
                  warnings=[])
    expect("layers_tilde", description="names with ~ stay flat without --split-layers",
           layers=sorted(["0", "Defpoints"] + layers), **common)
    expect("layers_tilde_split", description="--split-layers ~ builds S-FLOOR/edge and X/y/z",
           options={"splitLayers": "~"},
           layers=sorted(["0", "Defpoints", "S-FLOOR", "S-FLOOR/edge", "S-FLOOR/Floor hidden", "X", "X/y", "X/y/z"]),
           **common)


def gen_xdata_sublayers():
    doc = new_doc("R2004")
    doc.appids.add("TBCAD")
    msp = doc.modelspace()
    # Children come before their parents on purpose.
    leaf = doc.layers.add("A~child~leaf", color=1)
    leaf.set_xdata("TBCAD", [(1000, "parent=A~child"), (1000, "name=leaf"), (1070, 1)])
    child = doc.layers.add("A~child", color=2)
    child.set_xdata("TBCAD", [(1000, "parent=A"), (1000, "name=child"), (1070, 7)])
    doc.layers.add("A", color=3)
    orphan = doc.layers.add("orphan~x", color=4)
    orphan.set_xdata("TBCAD", [(1000, "parent=missing"), (1000, "name=x"), (1070, 0)])
    for i, n in enumerate(["A~child~leaf", "A~child", "A", "orphan~x"]):
        msp.add_line((0, i), (5, i), dxfattribs={"layer": n})
    save_ezdxf(doc, "xdata_sublayers", description="sub-layers restored from TBCAD XDATA, children before parents",
               exit=1, version="AC1018", encoding="ANSI_1252", dxf={"LINE": 4}, kinds={"Line": 4},
               layers=sorted(["0", "A", "A/child", "A/child/leaf", "orphan~x"]),
               warnings=["dxf.reader.sublayer_parent_missing"])


def gen_thai():
    # R2007+ files are UTF-8.
    doc = new_doc("R2007")
    doc.layers.add(THAI_WALL, color=1)
    msp = doc.modelspace()
    msp.add_text(f"{THAI_HELLO} TBCAD", dxfattribs={"height": 2.5, "layer": THAI_WALL, "insert": (0, 0)})
    msp.add_mtext(f"{THAI_BEDROOM}\\P{THAI_BATH}", dxfattribs={"insert": (0, 10), "char_height": 2.5})
    save_ezdxf(doc, "r2007_utf8_thai", description="R2007 UTF-8 with Thai text and a Thai layer name",
               exit=0, version="AC1021", encoding="UTF-8", dxf={"TEXT": 1, "MTEXT": 1},
               kinds={"Text": 1, "MText": 1}, layers=sorted(["0", THAI_WALL]),
               texts=sorted([f"{THAI_HELLO} TBCAD", f"{THAI_BEDROOM}\n{THAI_BATH}"]), warnings=[])

    # R2013 written by ezdxf also says ANSI_1252 in the header although the bytes are UTF-8.
    doc = new_doc("R2013")
    msp = doc.modelspace()
    msp.add_text(f"{THAI_BEDROOM} 1", dxfattribs={"height": 2.5})
    msp.add_line((0, 0), (5, 5))
    save_ezdxf(doc, "r2013_utf8_thai_stale_header", description="R2013 UTF-8 whose $DWGCODEPAGE still says ANSI_1252",
               exit=0, version="AC1027", encoding="UTF-8", dxf={"TEXT": 1, "LINE": 1},
               kinds={"Text": 1, "Line": 1}, layers=["0"], texts=[f"{THAI_BEDROOM} 1"], warnings=[])

    doc = new_doc("R2000")
    doc.encoding = "cp874"
    doc.layers.add(THAI_WALL, color=2)
    msp = doc.modelspace()
    msp.add_text(f"{THAI_HELLO} CAD", dxfattribs={"height": 2.5, "layer": THAI_WALL})
    msp.add_mtext(f"{THAI_BEDROOM}\\P{THAI_BATH}", dxfattribs={"insert": (0, 10), "char_height": 2.5})
    save_ezdxf(doc, "thai_cp874", description="R2000 with $DWGCODEPAGE ANSI_874 and cp874 bytes",
               exit=0, version="AC1015", encoding="ANSI_874", dxf={"TEXT": 1, "MTEXT": 1},
               kinds={"Text": 1, "MText": 1}, layers=sorted(["0", THAI_WALL]),
               texts=sorted([f"{THAI_HELLO} CAD", f"{THAI_BEDROOM}\n{THAI_BATH}"]), warnings=[])

    # Same bytes but with the $DWGCODEPAGE variable removed: the reader has to guess (Thai).
    path = os.path.join(OUT, "thai_cp874.dxf")
    data = open(path, "rb").read()
    marker = b"  9\n$DWGCODEPAGE\n  3\nANSI_874\n"
    assert marker in data
    write("thai_cp874_no_codepage.dxf", data.replace(marker, b""))
    expect("thai_cp874_no_codepage", description="cp874 bytes without $DWGCODEPAGE: encoding is guessed",
           exit=0, version="AC1015", encoding="ANSI_874", encodingGuessed=True,
           dxf={"TEXT": 1, "MTEXT": 1},
           kinds={"Text": 1, "MText": 1}, layers=sorted(["0", THAI_WALL, "Defpoints"]),
           texts=sorted([f"{THAI_HELLO} CAD", f"{THAI_BEDROOM}\n{THAI_BATH}"]), warnings=[],
           ezdxf=False)


def gen_blocks():
    doc = new_doc("R2013")
    msp = doc.modelspace()
    inner = doc.blocks.new("INNER")
    inner.add_circle((0, 0), 1)
    outer = doc.blocks.new("OUTER")
    outer.add_line((0, 0), (5, 0))
    outer.add_blockref("INNER", (2, 2))
    attr = doc.blocks.new("TAGGED")
    attr.add_line((0, 0), (2, 0))
    attr.add_attdef("TAG", (0, 1), "default", dxfattribs={"height": 1})
    msp.add_blockref("OUTER", (10, 10))
    msp.add_blockref("INNER", (0, 0), dxfattribs={"row_count": 2, "column_count": 3,
                                                  "row_spacing": 10, "column_spacing": 10})
    msp.add_blockref("TAGGED", (30, 30)).add_auto_attribs({"TAG": "V1"})
    msp.add_line((0, 0), (1, 1))
    save_ezdxf(doc, "r2013_blocks", description="R2013 nested blocks, a MINSERT array and an attribute",
               exit=0, version="AC1027", encoding="UTF-8",
               dxf={"CIRCLE": 1, "LINE": 3, "INSERT": 4, "ATTDEF": 1, "ATTRIB": 1},
               kinds={"Insert": 8, "Line": 1, "Text": 1}, layers=["0"], texts=["V1"], warnings=[])


def gen_r2018():
    doc = new_doc("R2018")
    msp = doc.modelspace()
    msp.add_line((0, 0), (10, 10))
    msp.add_circle((5, 5), 3)
    msp.add_lwpolyline([(0, 0), (4, 0), (4, 4)], close=True)
    msp.add_text("R2018", dxfattribs={"height": 2})
    save_ezdxf(doc, "r2018_basic", description="R2018 (AC1032) basic drawing", exit=0, version="AC1032",
               encoding="UTF-8", dxf={"LINE": 1, "CIRCLE": 1, "LWPOLYLINE": 1, "TEXT": 1},
               kinds={"Line": 1, "Circle": 1, "Polyline": 1, "Text": 1}, layers=["0"],
               texts=["R2018"], warnings=[])


def gen_r2004_basic():
    doc = new_doc("R2004")
    msp = doc.modelspace()
    msp.add_line((0, 0), (10, 10))
    msp.add_arc((0, 0), 5, 0, 90)
    msp.add_text("R2004", dxfattribs={"height": 2})
    save_ezdxf(doc, "r2004_basic", description="R2004 (AC1018) basic drawing", exit=0, version="AC1018",
               encoding="ANSI_1252", dxf={"LINE": 1, "ARC": 1, "TEXT": 1},
               kinds={"Line": 1, "Arc": 1, "Text": 1}, layers=["0"], texts=["R2004"], warnings=[])


def gen_binary():
    pairs = (header("AC1015", 4)
             + layer_table(("0", 7), ("A", 1))
             + section("ENTITIES",
                       line("A", 0.0, 0.0, 10.0, 5.0) + line("0", 1.0, 1.0, 2.0, 2.0)
                       + [(0, "CIRCLE"), (8, "A"), (10, 5.0), (20, 5.0), (30, 0.0), (40, 2.0)]
                       + [(0, "TEXT"), (8, "0"), (10, 1.0), (20, 1.0), (30, 0.0), (40, 2.5), (1, "bin")])
             + eof())
    write("binary_r2000.dxf", binary_dxf(pairs))
    expect("binary_r2000", description="binary DXF, R2000 style 2-byte group codes", exit=0, version="AC1015",
           binary=True, dxf={"LINE": 2, "CIRCLE": 1, "TEXT": 1}, unitScale=1.0,
           kinds={"Line": 2, "Circle": 1, "Text": 1}, layers=["0", "A"], texts=["bin"], warnings=[],
           ezdxf=False)


def gen_extrusion():
    doc = new_doc("R2000")
    msp = doc.modelspace()
    neg = {"extrusion": (0, 0, -1)}
    msp.add_arc((10, 0), 5, 30, 120, dxfattribs=dict(neg))
    msp.add_circle((3, 4), 2, dxfattribs=dict(neg))
    lw = msp.add_lwpolyline([(1, 0, 0, 0, 0), (2, 0, 0, 0, 1), (2, 3, 0, 0, 0)], format="xyseb",
                            dxfattribs=dict(neg))
    t = msp.add_text("neg", dxfattribs={"height": 1, "rotation": 30, "extrusion": (0, 0, -1)})
    t.set_placement((5, 5))
    blk = doc.blocks.new("TRI")
    blk.add_line((0, 0), (1, 0))
    blk.add_line((1, 0), (0, 1))
    msp.add_blockref("TRI", (7, 1), dxfattribs={"rotation": 90, "extrusion": (0, 0, -1)})
    msp.add_ellipse((0, 0), major_axis=(4, 0), ratio=0.5, start_param=0, end_param=math.pi / 2,
                    dxfattribs={"extrusion": (0, 0, -1)})
    h = msp.add_hatch(color=1, dxfattribs={"extrusion": (0, 0, -1)})
    h.paths.add_polyline_path([(0, 10), (4, 10), (4, 14), (0, 14)], is_closed=True)
    msp.add_arc((0, 0), 1, 0, 90, dxfattribs={"extrusion": (0.5, 0.0, 0.8660254037844386)})
    save_ezdxf(doc, "extrusion_neg_z", description="entities with extrusion (0,0,-1); one tilted arc is skipped",
               exit=1, version="AC1015", encoding="ANSI_1252",
               dxf={"ARC": 2, "CIRCLE": 1, "LWPOLYLINE": 1, "TEXT": 1, "INSERT": 1, "LINE": 2,
                    "ELLIPSE": 1, "HATCH": 1},
               kinds={"Arc": 1, "Circle": 1, "Polyline": 1, "Text": 1, "Insert": 1, "Ellipse": 1,
                      "Hatch": 1},
               layers=["0"], texts=["neg"], warnings=["dxf.reader.extrusion_3d"])


def gen_far():
    pairs = (header("AC1015", 4) + layer_table(("0", 7))
             + section("ENTITIES",
                       line("0", 0.0, 0.0, 100.0, 100.0)
                       + line("0", 5e8, 5e8, 5e8 + 1, 5e8)
                       + line("0", 1e12, 1e12, 1e12 + 1, 1e12))
             + eof())
    write("far_coordinates.dxf", ascii_dxf(pairs))
    expect("far_coordinates", description="one line beyond the working area (kept), one beyond the hard cap (dropped)",
           exit=1, version="AC1015", unitScale=1.0, dxf={"LINE": 3}, kinds={"Line": 2}, layers=["0"],
           warnings=["dxf.reader.beyond_hard_cap", "dxf.reader.outside_work_area"], ezdxf=False)


def gen_inches():
    pairs = (header("AC1015", 1) + layer_table(("0", 7))
             + section("ENTITIES",
                       line("0", 0.0, 0.0, 1.0, 2.0)
                       + [(0, "CIRCLE"), (8, "0"), (10, 3.0), (20, 3.0), (30, 0.0), (40, 0.5)]
                       + [(0, "TEXT"), (8, "0"), (10, 0.0), (20, 0.0), (30, 0.0), (40, 0.1), (1, "in")])
             + eof())
    write("units_inches.dxf", ascii_dxf(pairs))
    expect("units_inches", description="$INSUNITS = 1: every length is multiplied by 25.4", exit=0,
           version="AC1015", unitScale=25.4, dxf={"LINE": 1, "CIRCLE": 1, "TEXT": 1},
           kinds={"Line": 1, "Circle": 1, "Text": 1}, layers=["0"], texts=["in"], warnings=[], ezdxf=False)


def gen_proxy():
    proxy = [(0, "ACAD_PROXY_ENTITY"), (8, "0"), (90, 498), (91, 500), (92, 4), (310, bytes([1, 2, 3, 4])),
             (93, 2), (310, bytes([9, 8]))]
    pairs = (header("AC1015", 4) + layer_table(("0", 7))
             + section("ENTITIES", line("0", 0.0, 0.0, 5.0, 5.0) + proxy) + eof())
    # Binary so the 310 groups need no hex text.
    write("proxy_entity.dxf", binary_dxf(pairs))
    expect("proxy_entity", description="ACAD_PROXY_ENTITY is kept as a read-only proxy", exit=1,
           version="AC1015", binary=True, dxf={"LINE": 1, "ACAD_PROXY_ENTITY": 1},
           kinds={"Line": 1, "Proxy": 1}, layers=["0"], warnings=["dxf.reader.proxy_entity"], ezdxf=False)


def gen_unknown():
    def raw(t, *extra):
        return [(0, t), (8, "0")] + list(extra)

    ents = (line("0", 0.0, 0.0, 1.0, 1.0) + raw("3DSOLID", (100, "AcDbModelerGeometry"))
            + raw("REGION") + raw("RAY", (10, 0.0), (20, 0.0), (30, 0.0), (11, 1.0), (21, 1.0), (31, 0.0))
            + raw("IMAGE") + raw("3DSOLID") + raw("MLINE"))
    pairs = header("AC1015", 4) + layer_table(("0", 7)) + section("ENTITIES", ents) + eof()
    write("unknown_entities.dxf", ascii_dxf(pairs))
    expect("unknown_entities", description="entity types TBCAD does not read are skipped and counted",
           exit=1, version="AC1015", dxf={"LINE": 1, "3DSOLID": 2, "REGION": 1, "RAY": 1, "IMAGE": 1, "MLINE": 1},
           kinds={"Line": 1}, layers=["0"], warnings=["dxf.reader.skipped_entity"], ezdxf=False)


def gen_mtext_markup():
    texts_in = [
        "Line1\\PLine2",
        "{\\fArial|b1|i0|c0|p34;Bold} plain",
        "{\\H5;Big} {\\H2x;Rel}",
        "{\\C1;Red}",
        "\\S1/2; and \\Lunder\\l",
        "Dia %%c25 \\\\ \\{x\\}",
        "{\\Qc;\\W0.8;plain}",
    ]
    texts_out = [
        "Line1\nLine2",
        "{\\b1;Bold} plain",
        "{\\H5;Big} {\\H2x;Rel}",
        "{\\C1;Red}",
        "\\S1/2; and \\Lunder\\l",
        "Dia \u230025 \\\\ \\{x\\}",
        "plain",
    ]
    ents = []
    for i, s in enumerate(texts_in):
        ents += [(0, "MTEXT"), (8, "0"), (10, 0.0), (20, 5.0 * i), (30, 0.0), (40, 2.5), (71, 1), (1, s)]
    pairs = header("AC1015", 4) + layer_table(("0", 7)) + section("ENTITIES", ents) + eof()
    write("mtext_markup.dxf", ascii_dxf(pairs))
    expect("mtext_markup", description="MTEXT formatting codes: kept, flattened or dropped", exit=0,
           version="AC1015", dxf={"MTEXT": len(texts_in)}, kinds={"MText": len(texts_in)}, layers=["0"],
           texts=sorted(texts_out), warnings=[], ezdxf=False)


def gen_hatch_variants():
    doc = new_doc("R2000")
    msp = doc.modelspace()
    h = msp.add_hatch(color=1)
    h.paths.add_polyline_path([(0, 0, 0), (10, 0, 0.5), (10, 10, 0), (0, 10, 0)], is_closed=True)
    h = msp.add_hatch(color=2)
    p = h.paths.add_edge_path()
    p.add_line((20, 0), (30, 0))
    p.add_arc((30, 5), 5, 270, 90, ccw=True)
    p.add_line((30, 10), (20, 10))
    p.add_arc((20, 5), 5, 90, 270, ccw=True)
    h = msp.add_hatch(color=3)
    p = h.paths.add_edge_path()
    p.add_ellipse((50, 5), major_axis=(5, 0), ratio=0.6)
    h = msp.add_hatch(color=4)
    p = h.paths.add_edge_path()
    p.add_spline(control_points=[(70, 0), (72, 6), (78, 6), (80, 0)], degree=3)
    p.add_line((80, 0), (70, 0))
    h = msp.add_hatch(color=5)
    h.set_pattern_fill("ANSI31", scale=2.0, angle=30)
    h.paths.add_polyline_path([(90, 0), (100, 0), (100, 10), (90, 10)], is_closed=True)
    h.paths.add_polyline_path([(93, 3), (97, 3), (97, 7), (93, 7)], is_closed=True)
    save_ezdxf(doc, "hatch_variants", description="hatches with polyline, line+arc, ellipse, spline edges and an island",
               exit=0, version="AC1015", encoding="ANSI_1252", dxf={"HATCH": 5}, kinds={"Hatch": 5},
               layers=["0"], warnings=[])


def gen_broken():
    good = ascii_dxf(header("AC1015", 4) + layer_table(("0", 7)) + section(
        "ENTITIES", line("0", 0.0, 0.0, 1.0, 1.0) + line("0", 1.0, 1.0, 2.0, 0.0)
        + line("0", 2.0, 0.0, 3.0, 3.0) + line("0", 3.0, 3.0, 4.0, 0.0)) + eof())
    cut = good.index("  0\nLINE", good.rindex("  0\nLINE")) + len("  0\nLINE\n  8\n0\n 10\n3.0\n 20\n")
    write("broken_truncated.dxf", good[:cut])
    expect("broken_truncated", description="file cut inside the last LINE: no EOF, three lines survive",
           exit=1, version="AC1015", dxf_min={"LINE": 3}, kinds_min={"Line": 3}, warnings_has=["dxf.lexer.missing_eof"],
           ezdxf=False)

    lines = good.split("\n")
    noisy = []
    seen = 0
    for i, l in enumerate(lines):
        if l == "LINE" and lines[i - 1].strip() == "0":
            seen += 1
            if seen in (2, 3):
                noisy.insert(len(noisy) - 1, "")
                noisy.insert(len(noisy) - 1, "junk text that is not a group code")
        noisy.append(l)
    write("broken_garbage.dxf", "\n".join(noisy))
    expect("broken_garbage", description="blank lines, junk text and a decimal comma inside the entities",
           exit=1, version="AC1015", dxf_min={"LINE": 3}, kinds_min={"Line": 3}, ezdxf=False)

    pairs = (header("AC1009") + section("ENTITIES", line("STRAY", 0.0, 0.0, 1.0, 1.0)
                                        + line("ALSO", 1.0, 1.0, 2.0, 2.0) + line("stray", 2.0, 2.0, 3.0, 3.0))
             + eof())
    write("broken_no_tables.dxf", ascii_dxf(pairs))
    expect("broken_no_tables", description="no TABLES section: layers used by entities are created",
           exit=1, version="AC1009", dxf={"LINE": 3}, kinds={"Line": 3}, layers=["0", "ALSO", "STRAY"],
           warnings=["dxf.reader.missing_layer"], ezdxf=False)

    write("broken_not_dxf.dxf", "this is not a DXF file\nit has two lines\n")
    expect("broken_not_dxf", description="plain text is rejected", exit=2, ezdxf=False)

    write("broken_empty.dxf", "")
    expect("broken_empty", description="an empty file is rejected", exit=2, ezdxf=False)

    pairs = header("AC1006") + section("ENTITIES", line("0", 0.0, 0.0, 1.0, 1.0)) + eof()
    write("broken_old_version.dxf", ascii_dxf(pairs))
    expect("broken_old_version", description="AC1006 (R10) is older than R12 and is refused with a clear message",
           exit=2, ezdxf=False)

    pairs = (header("AC1015", 4) + layer_table(("0", 7))
             + section("ENTITIES", line("0", 0.0, 0.0, 1.0, 1.0)
                       + [(0, "CIRCLE"), (8, "0"), (10, 0.0), (20, 0.0), (30, 0.0), (40, -3.0)]
                       + [(0, "ARC"), (8, "0"), (10, 0.0), (20, 0.0), (30, 0.0), (40, 0.0), (50, 0.0), (51, 90.0)]
                       + [(0, "LWPOLYLINE"), (8, "0"), (90, 1), (70, 0), (10, 1.0), (20, 1.0)]
                       + [(0, "INSERT"), (8, "0"), (2, "NOSUCH"), (10, 0.0), (20, 0.0), (30, 0.0)])
             + eof())
    write("broken_entities.dxf", ascii_dxf(pairs))
    expect("broken_entities", description="degenerate entities and an INSERT of a block that does not exist",
           exit=1, version="AC1015", dxf={"LINE": 1, "CIRCLE": 1, "ARC": 1, "LWPOLYLINE": 1, "INSERT": 1},
           kinds={"Line": 1, "Insert": 1}, layers=["0"],
           warnings=["dxf.reader.bad_entity", "dxf.reader.missing_block"], ezdxf=False)


def main():
    os.makedirs(OUT, exist_ok=True)
    for fn in [gen_r12, gen_r2000_entities, gen_dimensions, gen_layers, gen_tilde_layers,
               gen_xdata_sublayers, gen_thai, gen_blocks, gen_r2018, gen_r2004_basic, gen_binary,
               gen_extrusion, gen_far, gen_inches, gen_proxy, gen_unknown, gen_mtext_markup,
               gen_hatch_variants, gen_broken]:
        fn()
    print("corpus written to", os.path.normpath(OUT))


if __name__ == "__main__":
    sys.exit(main())
