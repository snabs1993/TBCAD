#!/usr/bin/env python3
"""Summarise the drawing content of a PDF for plot-fidelity comparison (stdlib only).

Usage: python pdfstats.py file.pdf [more.pdf ...] > stats.txt

For every page it reports: page size, line widths used by strokes (with stroke colour),
dash patterns, fill colours and how often a fill is followed by a stroke of the same path,
text operators and fonts (selectable text vs drawn geometry), clip paths, images, and
transparency (ExtGState CA/ca). Everything is counted, not dumped, so the output stays small.
"""
import re
import sys
import zlib
from collections import Counter

NUM = rb"[-+]?(?:\d+\.?\d*|\.\d+)"


def objects(data):
    for m in re.finditer(rb"(\d+)\s+(\d+)\s+obj\b(.*?)endobj", data, re.S):
        yield int(m.group(1)), m.group(3)


def stream_of(body):
    m = re.search(rb"stream\r?\n", body)
    if not m:
        return None, body
    head = body[: m.start()]
    end = body.rfind(b"endstream")
    raw = body[m.end():end]
    if raw.endswith(b"\r\n"):
        raw = raw[:-2]
    elif raw.endswith(b"\n"):
        raw = raw[:-1]
    if b"/FlateDecode" in head:
        try:
            raw = zlib.decompress(raw)
        except zlib.error:
            try:
                raw = zlib.decompressobj().decompress(raw)
            except zlib.error:
                return head, b""
    return head, raw


def tokens(content):
    # Strings, arrays and names are kept whole enough for counting.
    pat = re.compile(rb"\((?:\\.|[^\\)])*\)|<[0-9A-Fa-f\s]*>|\[|\]|/[^\s/\[\]()<>]+|" + NUM + rb"|[A-Za-z'\"*]+")
    return pat.findall(content)


def analyse(content, out):
    st = []
    width = 1.0
    stroke = "?"
    fill = "?"
    dash = "[] 0"
    widths = Counter()
    dashes = Counter()
    fills = Counter()
    ops = Counter()
    fonts = Counter()
    path_ops = 0
    in_array = None
    for t in tokens(content):
        if t == b"[":
            in_array = []
            continue
        if t == b"]":
            st.append(b"[" + b" ".join(in_array or []) + b"]")
            in_array = None
            continue
        if in_array is not None:
            in_array.append(t)
            continue
        if re.fullmatch(NUM, t) or t[:1] in b"(</":
            st.append(t)
            continue
        op = t.decode("latin1")
        ops[op] += 1
        a = st
        st = []
        if op == "w" and a:
            width = float(a[-1])
        elif op == "d" and len(a) >= 2:
            dash = a[-2].decode("latin1") + " " + a[-1].decode("latin1")
        elif op in ("RG", "G", "K", "SC", "SCN") and a:
            stroke = " ".join(x.decode("latin1") for x in a[-4:])
        elif op in ("rg", "g", "k", "sc", "scn") and a:
            fill = " ".join(x.decode("latin1") for x in a[-4:])
        elif op in ("m", "l", "c", "v", "y", "re", "h"):
            path_ops += 1
        elif op in ("S", "s"):
            widths[(round(width, 4), stroke)] += 1
            dashes[dash] += 1
        elif op in ("f", "F", "f*"):
            fills[("fill", fill)] += 1
        elif op in ("B", "B*", "b", "b*"):
            fills[("fill+stroke", fill)] += 1
            widths[(round(width, 4), stroke)] += 1
            dashes[dash] += 1
        elif op == "Tf" and len(a) >= 2:
            fonts[a[-2].decode("latin1")] += 1
    out.append("  operators: " + ", ".join(f"{k}={v}" for k, v in ops.most_common(30)))
    out.append(f"  path construction ops: {path_ops}")
    out.append("  stroke widths (width, colour) x count:")
    for (w, c), n in sorted(widths.items(), key=lambda x: -x[1])[:40]:
        out.append(f"    w={w:<8} RG={c:<24} {n}")
    out.append("  dash patterns x strokes:")
    for d, n in dashes.most_common(25):
        out.append(f"    {d:<40} {n}")
    out.append("  fills (kind, colour) x count:")
    for (k, c), n in fills.most_common(25):
        out.append(f"    {k:<12} {c:<24} {n}")
    out.append("  fonts used by Tf: " + (", ".join(f"{k}={v}" for k, v in fonts.most_common(20)) or "none"))
    out.append(f"  text: BT={ops['BT']} Tj={ops['Tj']} TJ={ops['TJ']}  clip: W={ops['W']} W*={ops['W*']}  images/xobjects: Do={ops['Do']}")


def main():
    for path in sys.argv[1:]:
        data = open(path, "rb").read()
        print(f"== {path} ({len(data)} bytes)")
        boxes = re.findall(rb"/MediaBox\s*\[\s*(" + NUM + rb")\s+(" + NUM + rb")\s+(" + NUM + rb")\s+(" + NUM + rb")", data)
        for b in boxes[:4]:
            w = (float(b[2]) - float(b[0])) / 72 * 25.4
            h = (float(b[3]) - float(b[1])) / 72 * 25.4
            print(f"  MediaBox {w:.1f} x {h:.1f} mm")
        gs = Counter()
        for m in re.finditer(rb"/(CA|ca)\s+(" + NUM + rb")", data):
            gs[(m.group(1).decode(), m.group(2).decode())] += 1
        print("  ExtGState alpha: " + (", ".join(f"{k}={v}:{n}" for (k, v), n in gs.items()) or "none"))
        fontnames = Counter(m.group(1).decode("latin1") for m in re.finditer(rb"/BaseFont\s*/([^\s/\]>]+)", data))
        print("  embedded/base fonts: " + (", ".join(f"{k}x{v}" for k, v in fontnames.most_common(20)) or "none"))
        n = 0
        for num, body in objects(data):
            head, content = stream_of(body)
            if head is None or not content:
                continue
            if b"/Subtype" in head and b"/Image" in head:
                print(f"  image object {num}: {head[:120]!r}")
                continue
            if not re.search(rb"\b(re|l|m|c)\s", content[:200000]):
                continue
            n += 1
            out = [f" content stream obj {num}: {len(content)} bytes"]
            analyse(content, out)
            print("\n".join(out))
        print(f"  content streams analysed: {n}")


if __name__ == "__main__":
    main()
