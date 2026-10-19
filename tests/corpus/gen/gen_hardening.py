#!/usr/bin/env python3
"""Generates the real-world problem files of tests/corpus/reader (proxy graphics, AEC objects,
bad encodings, non-finite and huge numbers, broken handles, tables that are missing).

Pure Python (no ezdxf): the DXF text is written by hand so that every defect is exactly the one
meant. Everything is made by the TBCAD project. The expected files record what the generator put
into each drawing, not what the reader gave back.

    python3 tests/corpus/gen/gen_hardening.py
"""
import json
import math
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reader")


def write(name, data):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)


def expect(name, **fields):
    with open(os.path.join(OUT, name + ".expected.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(fields, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")


def grp(code, value):
    if isinstance(value, bytes):
        return b"%3d\n" % code + value + b"\n"
    return ("%3d\n%s\n" % (code, value)).encode("utf-8")


def header(ver, codepage=None, insunits=4):
    out = grp(0, "SECTION") + grp(2, "HEADER") + grp(9, "$ACADVER") + grp(1, ver)
    if codepage:
        out += grp(9, "$DWGCODEPAGE") + grp(3, codepage)
    out += grp(9, "$INSUNITS") + grp(70, insunits) + grp(0, "ENDSEC")
    return out


def layer_table(*names):
    out = grp(0, "SECTION") + grp(2, "TABLES") + grp(0, "TABLE") + grp(2, "LAYER") + grp(70, len(names))
    for n in names:
        out += grp(0, "LAYER") + grp(2, n) + grp(70, 0) + grp(62, 7) + grp(6, "CONTINUOUS")
    out += grp(0, "ENDTAB") + grp(0, "ENDSEC")
    return out


def entities(*records):
    return grp(0, "SECTION") + grp(2, "ENTITIES") + b"".join(records) + grp(0, "ENDSEC")


EOF_ = grp(0, "EOF")


def line(x1, y1, x2, y2, layer="0"):
    return grp(0, "LINE") + grp(8, layer) + grp(10, x1) + grp(20, y1) + grp(11, x2) + grp(21, y2)


# ---- proxy graphics -------------------------------------------------------------------------

def rec(t, payload):
    return struct.pack("<II", len(payload) + 8, t) + payload


def v3(x, y, z=0.0):
    return struct.pack("<3d", x, y, z)


def gfx(*records):
    body = b"".join(records)
    return struct.pack("<II", len(body) + 8, len(records)) + body


def padded(b):
    b = b + b"\0"
    while len(b) % 4:
        b += b"\0"
    return b


def hexchunks(code, data):
    out = b""
    for i in range(0, len(data), 127):
        out += grp(code, data[i:i + 127].hex().upper())
    return out


SQUARE = rec(6, struct.pack("<I", 4) + v3(0, 0) + v3(10, 0) + v3(10, 10) + v3(0, 10))
CIRCLE = rec(2, v3(5, 5) + struct.pack("<d", 2) + v3(0, 0, 1))
ARC = rec(4, v3(20, 0) + struct.pack("<d", 5) + v3(0, 0, 1) + v3(1, 0) + struct.pack("<d", math.pi) + struct.pack("<I", 0))
RED = rec(14, struct.pack("<I", 1))
TRUE_GREEN = rec(22, struct.pack("<I", 0xC2000000 | (0 << 16) | (200 << 8) | 0))
FILL_ON = rec(20, struct.pack("<I", 1))
TRI_FILL = rec(7, struct.pack("<I", 3) + v3(30, 0) + v3(40, 0) + v3(35, 8))
SHELL = rec(9, struct.pack("<I", 4) + v3(50, 0) + v3(60, 0) + v3(60, 10) + v3(50, 10)
            + struct.pack("<I", 5) + struct.pack("<iIIII", 4, 0, 1, 2, 3))


def text_rec(x, y, h, s, enc):
    return rec(10, v3(x, y) + v3(0, 0, 1) + v3(1, 0) + struct.pack("<3d", h, 1.0, 0.0) + padded(s.encode(enc)))


def utext_rec(x, y, h, s):
    return rec(36, v3(x, y) + v3(0, 0, 1) + v3(1, 0) + struct.pack("<3d", h, 1.0, 0.0)
               + (s.encode("utf-16-le") + b"\0\0" + (b"\0\0" if (len(s) * 2 + 2) % 4 else b"")))


def proxy_entity(graphics, entity_data=b"\x01\x02\x03\x04", cls="AEC_WALL"):
    return (grp(0, "ACAD_PROXY_ENTITY") + grp(8, "0") + grp(100, "AcDbProxyEntity")
            + grp(90, 498) + grp(91, 500) + grp(92, len(graphics)) + hexchunks(310, graphics)
            + grp(93, len(entity_data)) + hexchunks(310, entity_data) + grp(95, 0) + grp(70, 0))


def foreign(dxftype, graphics, layer="0", extra=b""):
    return (grp(0, dxftype) + grp(8, layer) + extra + grp(160, len(graphics)) + hexchunks(310, graphics))


def make_proxy_graphics():
    g = gfx(RED, SQUARE, TRUE_GREEN, CIRCLE, ARC, FILL_ON, TRI_FILL, SHELL,
            text_rec(1, 12, 2.5, "ผนัง", "cp874"))
    data = header("AC1015", "ANSI_874") + layer_table("0") + entities(line(0, 0, 1, 1), proxy_entity(g)) + EOF_
    write("proxy_graphics.dxf", data)
    expect("proxy_graphics",
           description="ACAD_PROXY_ENTITY with proxy graphics is drawn from them (read-only); text in cp874",
           exit=1, ezdxf=False, version="AC1015", encoding="ANSI_874",
           dxf={"ACAD_PROXY_ENTITY": 1, "LINE": 1}, kinds={"Line": 1, "Proxy": 1, "Text": 1},
           texts=["ผนัง"], layers=["0"], warnings=["dxf.reader.proxy_entity"],
           proxy_items_min=5)


def make_aec():
    wall = gfx(RED, SQUARE, SHELL)
    solid = gfx(FILL_ON, TRI_FILL)
    station = gfx(utext_rec(3, 4, 2.0, "สถานี 0+000"), CIRCLE)
    data = (header("AC1027") + layer_table("0", "A-WALL")
            + entities(line(0, 0, 5, 5),
                       foreign("AEC_WALL", wall, "A-WALL", grp(100, "AcDbEntity")),
                       foreign("3DSOLID", solid, "0", grp(100, "AcDbModelerGeometry") + grp(70, 1) + grp(1, "ACIS data omitted")),
                       grp(0, "AEC_OPENING") + grp(8, "A-WALL") + grp(10, 1) + grp(20, 1),
                       foreign("AECC_ALIGNMENT", station))
            + EOF_)
    write("aec_objects.dxf", data)
    expect("aec_objects",
           description="AEC/Civil/ACIS objects carry proxy graphics and are shown read-only; one without graphics is kept as it is (not shown) and written back",
           exit=0, ezdxf=False, version="AC1027", encoding="UTF-8",
           dxf={"3DSOLID": 1, "AECC_ALIGNMENT": 1, "AEC_OPENING": 1, "AEC_WALL": 1, "LINE": 1},
           kinds={"Line": 1, "Proxy": 4, "Text": 1}, texts=["สถานี 0+000"],
           layers=["0", "A-WALL"], warnings=[], proxy_items_min=4)


def make_damaged():
    ok = gfx(SQUARE)
    broken = ok + struct.pack("<II", 5000, 6) + b"\x01\x02"  # record claims more bytes than exist
    junk = b"\xde\xad\xbe\xef" * 3
    tiny = struct.pack("<II", 8, 0)
    odd = gfx(rec(99, b"\x00" * 12), rec(6, struct.pack("<I", 2) + v3(0, 0)))  # unknown type, truncated polyline
    nan = gfx(rec(2, struct.pack("<3d", float("nan"), 0, 0) + struct.pack("<d", 1) + v3(0, 0, 1)))
    data = (header("AC1015", "ANSI_1252") + layer_table("0")
            + entities(proxy_entity(broken), proxy_entity(junk), proxy_entity(tiny), proxy_entity(odd),
                       proxy_entity(nan), line(0, 0, 3, 3))
            + EOF_)
    write("proxy_damaged.dxf", data)
    expect("proxy_damaged",
           description="damaged proxy graphics: what can be read is kept, the rest is reported, nothing crashes",
           exit=1, ezdxf=False, version="AC1015",
           dxf={"ACAD_PROXY_ENTITY": 5, "LINE": 1}, kinds={"Line": 1, "Proxy": 5},
           layers=["0"], warnings_has=["dxf.reader.proxy_entity", "dxf.reader.proxy_graphics_damaged"],
           proxy_items_min=1)


# ---- encodings --------------------------------------------------------------------------------

def make_bad_utf8():
    text_bad = b"ok \xe0\xb8\xaa\xe0\xb8\xa7 bad:\xff\xfe end \xe0\xb8"
    layer_bad = b"L-\xff\xfe-x"
    data = (header("AC1032") + layer_table("0", layer_bad)
            + entities(grp(0, "TEXT") + grp(8, layer_bad) + grp(10, 0) + grp(20, 0) + grp(40, 2.5) + grp(1, text_bad),
                       grp(0, "TEXT") + grp(8, "0") + grp(10, 0) + grp(20, 5) + grp(40, 2.5) + grp(1, "ปกติ \\U+0E01"),
                       line(0, 0, 1, 1))
            + EOF_)
    write("bad_utf8_r2018.dxf", data)
    expect("bad_utf8_r2018",
           description="R2018 file with invalid UTF-8 bytes in a text and a layer name: kept with replacement characters, reported",
           exit=1, ezdxf=False, version="AC1032",
           dxf={"LINE": 1, "TEXT": 2}, kinds={"Line": 1, "Text": 2},
           warnings_has=["dxf.encoding.mismatch"])


def make_mixed_encoding():
    # cp874 bytes in a file that claims ANSI_1252, plus \U+ escapes the codepage cannot hold.
    thai = "ห้องนอน".encode("cp874")
    data = (header("AC1009") + layer_table("0")
            + entities(grp(0, "TEXT") + grp(8, "0") + grp(10, 0) + grp(20, 0) + grp(40, 2.5) + grp(1, thai))
            + EOF_)
    write("thai_cp874_r12_no_codepage.dxf", data)
    expect("thai_cp874_r12_no_codepage",
           description="R12 file without $DWGCODEPAGE whose text is Thai cp874: the guess is Thai and is reported",
           exit=0, ezdxf=False, version="AC1009", encoding="ANSI_874", encodingGuessed=True,
           dxf={"TEXT": 1}, kinds={"Text": 1}, texts=["ห้องนอน"])


# ---- numbers ----------------------------------------------------------------------------------

def make_numbers():
    data = (header("AC1015", "ANSI_1252") + layer_table("0")
            + entities(
                line("nan", 0, 1, 1), line(0, "inf", 1, 1), line("1e999", 0, 5, 5), line("-1e400", 0, 5, 5),
                line("1,5", 0, 10, 10), line("  12.5  ", "\t3", 20, 0), line("", "", 2, 2), line("abc", 0, 3, 3),
                line("1e15", 0, 0, 0), line(5e11, 0, 0, 0), line("0x10", 0, 7, 7), line("1.2.3", 0, 4, 4),
                line(0, 0, 100, 100))
            + EOF_)
    write("numbers_hostile.dxf", data)
    expect("numbers_hostile",
           description="non-numbers, infinities, comma decimals, huge values (beyond the hard cap are dropped) and padding",
           exit=1, ezdxf=False, version="AC1015",
           dxf={"LINE": 13}, kinds_min={"Line": 5}, layers=["0"],
           warnings_has=["dxf.lexer.bad_number"])


# ---- handles ----------------------------------------------------------------------------------

def make_handles():
    def h(handle, owner):
        return (grp(5, handle) if handle is not None else b"") + (grp(330, owner) if owner is not None else b"")

    blocks = (grp(0, "SECTION") + grp(2, "BLOCKS")
              + grp(0, "BLOCK") + grp(5, "ZZZ") + grp(330, "FFFFFFFFFFFFFFFFFFFFFFFF") + grp(8, "0") + grp(2, "B1") + grp(70, 0) + grp(10, 0) + grp(20, 0) + grp(30, 0)
              + line(0, 0, 1, 0) + grp(0, "ENDBLK") + grp(5, "ZZZ") + grp(8, "0")
              + grp(0, "ENDSEC"))
    data = (header("AC1018", "ANSI_1252") + layer_table("0") + blocks
            + entities(
                grp(0, "LINE") + h("1F", "1F") + grp(8, "0") + grp(10, 0) + grp(20, 0) + grp(11, 1) + grp(21, 1),
                grp(0, "LINE") + h("1F", "0") + grp(8, "0") + grp(10, 0) + grp(20, 0) + grp(11, 2) + grp(21, 2),
                grp(0, "LINE") + h("not hex!", "99999") + grp(8, "0") + grp(10, 0) + grp(20, 0) + grp(11, 3) + grp(21, 3),
                grp(0, "LINE") + h("", "") + grp(8, "0") + grp(10, 0) + grp(20, 0) + grp(11, 4) + grp(21, 4),
                grp(0, "LINE") + h("F" * 400, "-7") + grp(8, "0") + grp(10, 0) + grp(20, 0) + grp(11, 5) + grp(21, 5),
                grp(0, "INSERT") + h("20", "1F") + grp(8, "0") + grp(2, "B1") + grp(10, 0) + grp(20, 0),
                grp(0, "INSERT") + h("1F", "7") + grp(8, "0") + grp(2, "NOPE") + grp(10, 0) + grp(20, 0),
                grp(0, "HATCH") + h("21", "1F") + grp(8, "0") + grp(2, "SOLID") + grp(70, 1) + grp(91, 1) + grp(92, 1)
                + grp(93, 3) + grp(72, 0) + grp(10, 0) + grp(20, 0) + grp(10, 4) + grp(20, 0) + grp(10, 4) + grp(20, 4)
                + grp(97, 2) + grp(330, "DEAD") + grp(330, "BEEF"),
                grp(0, "DIMENSION") + h("22", "1F") + grp(8, "0") + grp(2, "*D77") + grp(70, 0)
                + grp(10, 0) + grp(20, 5) + grp(13, 0) + grp(23, 0) + grp(14, 10) + grp(24, 0))
            + EOF_)
    write("broken_handles.dxf", data)
    expect("broken_handles",
           description="duplicate, non-hex, empty and enormous handles and owner pointers to nothing: handles are not needed to read the geometry",
           exit=1, ezdxf=False, version="AC1018",
           dxf_min={"LINE": 5}, kinds_min={"Line": 5}, layers=["0"],
           warnings_has=["dxf.reader.missing_block"])


# ---- tables -----------------------------------------------------------------------------------

def make_missing_tables():
    ents = (grp(0, "LINE") + grp(8, "Walls") + grp(6, "DASHED") + grp(10, 0) + grp(20, 0) + grp(11, 5) + grp(21, 5)
            + grp(0, "TEXT") + grp(8, "Notes") + grp(7, "Arial") + grp(10, 0) + grp(20, 0) + grp(40, 2.5) + grp(1, "label")
            + grp(0, "LEADER") + grp(8, "Notes") + grp(3, "ISO-25") + grp(10, 0) + grp(20, 0) + grp(10, 5) + grp(20, 5)
            + grp(0, "INSERT") + grp(8, "0") + grp(2, "Missing") + grp(10, 1) + grp(20, 1)
            + grp(0, "CIRCLE") + grp(8, "Walls") + grp(10, 2) + grp(20, 2) + grp(40, 1))
    # TABLES with only a VPORT table and an empty LAYER table; no LTYPE, STYLE, DIMSTYLE, BLOCK_RECORD.
    tables = (grp(0, "SECTION") + grp(2, "TABLES")
              + grp(0, "TABLE") + grp(2, "VPORT") + grp(70, 0) + grp(0, "ENDTAB")
              + grp(0, "TABLE") + grp(2, "LAYER") + grp(70, 0) + grp(0, "ENDTAB")
              + grp(0, "ENDSEC"))
    data = header("AC1021") + tables + entities(ents) + EOF_
    write("missing_tables_refs.dxf", data)
    expect("missing_tables_refs",
           description="TABLES lacks LTYPE, STYLE, DIMSTYLE, BLOCK_RECORD and the layers entities use: defaults are made and reported",
           exit=1, ezdxf=False, version="AC1021",
           dxf={"CIRCLE": 1, "INSERT": 1, "LEADER": 1, "LINE": 1, "TEXT": 1},
           kinds={"Circle": 1, "Insert": 1, "Leader": 1, "Line": 1, "Text": 1}, texts=["label"],
           layers=["0", "Notes", "Walls"],
           warnings_has=["dxf.reader.missing_layer", "dxf.reader.missing_linetype"])

    # No TABLES section at all and no HEADER.
    bare = entities(line(0, 0, 1, 1, "X"), grp(0, "CIRCLE") + grp(8, "X") + grp(10, 0) + grp(20, 0) + grp(40, 2)) + EOF_
    write("no_header_no_tables.dxf", bare)
    expect("no_header_no_tables",
           description="only an ENTITIES section: version unknown, layers created from use",
           exit=1, ezdxf=False, dxf={"CIRCLE": 1, "LINE": 1}, kinds={"Circle": 1, "Line": 1},
           layers=["0", "X"], warnings_has=["dxf.reader.missing_layer"])

    # Sections in the wrong order, ENTITIES before TABLES, a BLOCKS section that never ends.
    swapped = (header("AC1015", "ANSI_1252")
               + entities(grp(0, "LINE") + grp(8, "A") + grp(10, 0) + grp(20, 0) + grp(11, 1) + grp(21, 1))
               + layer_table("0", "A")
               + grp(0, "SECTION") + grp(2, "BLOCKS") + grp(0, "BLOCK") + grp(2, "B") + grp(70, 0) + grp(10, 0) + grp(20, 0) + grp(30, 0)
               + line(0, 0, 2, 2))
    write("sections_swapped_unclosed.dxf", swapped)
    expect("sections_swapped_unclosed",
           description="ENTITIES before TABLES and an unclosed BLOCKS section at the end of the file",
           exit=1, ezdxf=False, version="AC1015", dxf_min={"LINE": 1}, kinds_min={"Line": 1})


# ---- block cycles and blow-ups ------------------------------------------------------------------

def make_cycles():
    def block(name, body, flags=0, path=None):
        out = grp(0, "BLOCK") + grp(8, "0") + grp(2, name) + grp(70, flags) + grp(10, 0) + grp(20, 0) + grp(30, 0)
        if path:
            out += grp(1, path)
        return out + grp(3, name) + body + grp(0, "ENDBLK") + grp(8, "0")

    def insert(name, x=0, y=0, extra=b""):
        return grp(0, "INSERT") + grp(8, "0") + grp(2, name) + grp(10, x) + grp(20, y) + extra

    # Each level holds the next one twice: 2^60 lines in the end if nothing stops the expansion.
    levels = b"".join(block("L%d" % i, insert("L%d" % (i + 1)) + insert("L%d" % (i + 1), 1, 0))
                      for i in range(60))
    blocks = (grp(0, "SECTION") + grp(2, "BLOCKS")
              + block("SELF", line(0, 0, 1, 1) + insert("SELF", 2, 0))
              + block("PING", insert("PONG")) + block("PONG", insert("PING") + line(0, 0, 0, 1))
              + levels + block("L60", line(0, 0, 1, 0))
              + block("ME", b"", 4, "cycles.dxf")
              + grp(0, "ENDSEC"))
    big = grp(70, 30000) + grp(71, 30000) + grp(44, 1) + grp(45, 1)
    data = (header("AC1015", "ANSI_1252") + layer_table("0") + blocks
            + entities(insert("SELF"), insert("PING"), insert("L0"), insert("L59", 0, 0, big), insert("ME"),
                       line(0, 0, 5, 5))
            + EOF_)
    write("cycles.dxf", data)
    expect("cycles",
           description="a block that inserts itself, two that insert each other, 60 levels that each insert the next twice, a 30000 x 30000 MINSERT and an xref to the drawing itself",
           exit=1, ezdxf=False, version="AC1015", kinds_min={"Line": 1, "Insert": 3},
           warnings_has=["dxf.reader.entity_rejected"])


for fn in (make_proxy_graphics, make_aec, make_damaged, make_bad_utf8, make_mixed_encoding,
           make_numbers, make_handles, make_missing_tables, make_cycles):
    fn()
print("done")
