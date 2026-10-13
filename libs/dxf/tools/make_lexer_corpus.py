#!/usr/bin/env python3
"""Writes the hand-made lexer test files in tests/corpus/synthetic/lexer/.

Every file is built from a list of (group code, value) pairs, and the expected token
stream (<name>.expected.txt) is derived from that list, not from the C++ lexer, so the
tests check the lexer against an independent description. Files that are meant to be
damaged are written from literal text with expected output given by hand.

Expected file format, one record per line (all G lines first, then all W lines):
  G <line> <code> <S|D|I|B|H|X> <value>     a group (line = byte offset for binary DXF)
  W <line> <diagnostic code>                a warning or error, in order

Run from anywhere: python3 libs/dxf/tools/make_lexer_corpus.py
"""
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tests",
                   "corpus", "synthetic", "lexer")


def kind(code):
    if code <= 4 or 6 <= code <= 9 or code in (100, 102, 999) or 300 <= code <= 309 \
            or 1000 <= code <= 1003:
        return "S"
    if code == 5 or code == 105 or 320 <= code <= 369 or code == 1005:
        return "H"
    if 10 <= code <= 59 or 110 <= code <= 149 or 210 <= code <= 239 or 1010 <= code <= 1059:
        return "D"
    if 60 <= code <= 79 or 90 <= code <= 99 or 160 <= code <= 179 or 270 <= code <= 289 \
            or 370 <= code <= 389 or 420 <= code <= 429 or 1060 <= code <= 1071:
        return "I"
    if 290 <= code <= 299:
        return "B"
    if 310 <= code <= 319 or code == 1004:
        return "X"
    raise ValueError(code)


def fmt_double(v):
    s = repr(float(v))
    return s[:-2] if s.endswith(".0") else s


def escape(raw):
    """Printable ASCII stays, everything else (and backslash) becomes \\xNN."""
    return "".join(chr(b) if 0x20 <= b < 0x7F and b != 0x5C else "\\x%02X" % b for b in raw)


def expected_value(code, value):
    k = kind(code)
    if k == "D":
        return fmt_double(value)
    if k == "B":
        return "1" if value else "0"
    if k == "X":
        return value.hex().upper()
    if k == "H":
        return value
    if k == "I":
        return str(value)
    return escape(value if isinstance(value, bytes) else value.encode("utf-8"))


def ascii_text(pairs, eol="\n"):
    """Returns (bytes, expected lines). Numeric codes are right-aligned like AutoCAD."""
    out = b""
    exp = []
    line = 1
    for code, value in pairs:
        text = value if isinstance(value, bytes) else \
            (value.encode("utf-8") if isinstance(value, str) else None)
        k = kind(code)
        if k == "D":
            text = repr(float(value)).encode()
        elif k == "I":
            text = str(value).encode()
        elif k == "B":
            text = (b"1" if value else b"0")
        elif k == "X":
            text = value.hex().upper().encode()
        elif k == "H":
            text = value.encode()
        out += f"{code:3d}".encode() + eol.encode() + text + eol.encode()
        exp.append(f"G {line} {code} {k} {expected_value(code, value)}")
        line += 2
    return out, exp


def write(name, data, expected):
    # Groups first, then diagnostics (both in the order they occur).
    expected = [e for e in expected if e[0] == "G"] + [e for e in expected if e[0] == "W"]
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)
    with open(os.path.join(OUT, os.path.splitext(name)[0] + ".expected.txt"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write("\n".join(expected) + "\n")


def header(ver, codepage=None):
    p = [(0, "SECTION"), (2, "HEADER"), (9, "$ACADVER"), (1, ver)]
    if codepage:
        p += [(9, "$DWGCODEPAGE"), (3, codepage)]
    return p + [(0, "ENDSEC")]


def line_entity(x1, y1, x2, y2, layer="0"):
    return [(0, "LINE"), (8, layer), (10, x1), (20, y1), (30, 0.0), (11, x2), (21, y2),
            (31, 0.0)]


def entities(*items):
    pairs = [(0, "SECTION"), (2, "ENTITIES")]
    for it in items:
        pairs += it
    return pairs + [(0, "ENDSEC")]


EOF_PAIRS = [(0, "EOF")]


def main():
    # R12 ASCII, LF.
    pairs = header("AC1009") + entities(line_entity(0.0, 0.0, 100.0, 50.5)) + EOF_PAIRS
    data, exp = ascii_text(pairs)
    write("r12_ascii.dxf", data, exp)

    # R2000 ASCII, CRLF; every value type; comment; handles; binary chunk.
    pairs = header("AC1015", "ANSI_1252") + entities(
        [(999, "made by hand"), (0, "CIRCLE"), (5, "1F"), (100, "AcDbEntity"), (8, "WALL"),
         (62, 3), (370, -1), (100, "AcDbCircle"), (10, 12.5), (20, -7.25), (30, 0.0),
         (40, 3.0), (90, 70000), (160, 5000000000), (290, 1), (310, bytes([0, 1, 0xAB, 0xFF])),
         (1000, "xdata text"), (1071, 7)]) + EOF_PAIRS
    data, exp = ascii_text(pairs, "\r\n")
    write("r2000_ascii_crlf.dxf", data, exp)

    # R2013 UTF-8 with Thai text and a (wrong) ANSI_1252 codepage left in the header.
    pairs = header("AC1027", "ANSI_1252") + entities(
        [(0, "TEXT"), (5, "20"), (8, "ห้อง"), (10, 1.0), (20, 2.0), (30, 0.0), (40, 2.5),
         (1, "ห้องนอน"), (7, "STANDARD")]) + EOF_PAIRS
    data, exp = ascii_text(pairs)
    write("r2013_utf8_thai.dxf", data, exp)

    # R2000 Thai text in cp874, no $DWGCODEPAGE.
    thai = "ห้องนอน".encode("cp874")
    pairs = header("AC1015") + entities(
        [(0, "TEXT"), (8, "0"), (10, 0.0), (20, 0.0), (30, 0.0), (40, 2.5), (1, thai)]) \
        + EOF_PAIRS
    data, exp = ascii_text(pairs)
    write("r2000_cp874_thai.dxf", data, exp)

    # Mixed line endings: LF, CRLF and lone CR in one file.
    body = (b"  0\nSECTION\r\n  2\rENTITIES\n  0\r\nPOINT\n 10\r1.5\n 20\n2.5\r\n"
            b" 30\n0.0\r  0\nENDSEC\r\n  0\nEOF\r\n")
    exp = ["G 1 0 S SECTION", "G 3 2 S ENTITIES", "G 5 0 S POINT", "G 7 10 D 1.5",
           "G 9 20 D 2.5", "G 11 30 D 0", "G 13 0 S ENDSEC", "G 15 0 S EOF"]
    write("mixed_eol.dxf", body, exp)

    # No EOF marker; ends after ENDSEC with a trailing blank line.
    body = b"0\nSECTION\n2\nENTITIES\n0\nPOINT\n10\n1.0\n20\n2.0\n0\nENDSEC\n\n"
    exp = ["G 1 0 S SECTION", "G 3 2 S ENTITIES", "G 5 0 S POINT", "G 7 10 D 1",
           "G 9 20 D 2", "G 11 0 S ENDSEC", "W 14 dxf.lexer.missing_eof"]
    write("missing_eof.dxf", body, exp)

    # Garbage group code in the middle of the entities: resync at the next "0 CIRCLE".
    body = (b"0\nSECTION\n2\nENTITIES\n0\nLINE\n8\n0\n10\n1.0\nthis is not a code\n20\n2.0\n"
            b"junk\n0\nCIRCLE\n8\n0\n40\n5.0\n0\nENDSEC\n0\nEOF\n")
    exp = ["G 1 0 S SECTION", "G 3 2 S ENTITIES", "G 5 0 S LINE", "G 7 8 S 0",
           "G 9 10 D 1", "W 11 dxf.lexer.bad_group_code", "W 11 dxf.lexer.resync",
           "G 15 0 S CIRCLE", "G 17 8 S 0", "G 19 40 D 5", "G 21 0 S ENDSEC", "G 23 0 S EOF"]
    write("bad_group_code.dxf", body, exp)

    # BOM, leading spaces on codes, stray blank line, decimal comma, empty number.
    body = (b"\xef\xbb\xbf  0\nSECTION\n  2\nENTITIES\n  0\nPOINT\n\n 10\n1,5\n 20\n\n 30\n"
            b" 3.0  \n  0\nENDSEC\n  0\nEOF\n")
    exp = ["G 1 0 S SECTION", "G 3 2 S ENTITIES", "G 5 0 S POINT",
           "W 7 dxf.lexer.blank_group_code", "G 8 10 D 1.5", "W 9 dxf.lexer.decimal_comma",
           "G 10 20 D 0", "W 11 dxf.lexer.bad_number", "G 12 30 D 3", "G 14 0 S ENDSEC",
           "G 16 0 S EOF"]
    write("bom_blank_comma.dxf", body, exp)

    # Value missing at the end of the file (truncated), plus no EOF.
    body = b"0\nSECTION\n2\nENTITIES\n0\nPOINT\n10\n"
    exp = ["G 1 0 S SECTION", "G 3 2 S ENTITIES", "G 5 0 S POINT",
           "W 7 dxf.lexer.missing_value", "W 8 dxf.lexer.missing_eof"]
    write("truncated_value.dxf", body, exp)

    write_binary()


def write_binary():
    sentinel = b"AutoCAD Binary DXF\r\n\x1a\x00"

    def build(width, pairs):
        out = sentinel
        exp = []
        for code, value in pairs:
            off = len(out)
            if width == 1:
                out += bytes([code]) if code < 255 else b"\xff" + struct.pack("<h", code)
            else:
                out += struct.pack("<h", code)
            k = kind(code)
            if k == "S" or k == "H":
                out += value.encode("utf-8") + b"\x00"
            elif k == "D":
                out += struct.pack("<d", value)
            elif k == "I":
                size = 8 if 160 <= code <= 169 else 2 if (code < 90 or 170 <= code <= 289 or 370 <= code <= 389
                             or 400 <= code <= 409 or 1060 <= code <= 1070) else 4
                out += struct.pack({2: "<h", 4: "<i", 8: "<q"}[size], value)
            elif k == "B":
                out += bytes([1 if value else 0])
            elif k == "X":
                out += bytes([len(value)]) + value
            exp.append(f"G {off} {code} {k} {expected_value(code, value)}")
        return out, exp

    r12 = [(0, "SECTION"), (2, "HEADER"), (9, "$ACADVER"), (1, "AC1009"), (0, "ENDSEC"),
           (0, "SECTION"), (2, "ENTITIES"), (0, "LINE"), (8, "0"), (10, 1.5), (20, 2.5),
           (30, 0.0), (11, 100.0), (21, 50.0), (31, 0.0), (62, 7), (0, "ENDSEC"), (0, "EOF")]
    data, exp = build(1, r12)
    write("binary_r12.dxf", data, exp)

    r13 = [(0, "SECTION"), (2, "HEADER"), (9, "$ACADVER"), (1, "AC1015"), (0, "ENDSEC"),
           (0, "SECTION"), (2, "ENTITIES"), (0, "CIRCLE"), (5, "2A"), (8, "WALL"), (10, 1.5),
           (20, 2.5), (30, 0.0), (40, 3.0), (90, 70000), (290, 1), (310, bytes([1, 2, 255])),
           (1071, -3), (0, "ENDSEC"), (0, "EOF")]
    data, exp = build(2, r13)
    write("binary_r2000.dxf", data, exp)

    # Binary file cut in the middle of a double.
    data, _ = build(2, r13[:13])
    data = data[:-3]
    full, exp_full = build(2, r13[:12])
    exp = exp_full + ["W %d dxf.lexer.truncated" % len(full), "W %d dxf.lexer.missing_eof"
                      % 0]
    write("binary_truncated.dxf", data, exp)


if __name__ == "__main__":
    main()
