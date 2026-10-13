#!/usr/bin/env python3
"""Exports every corpus drawing with tbcad-convert and audits the result with ezdxf (W4-6).

    python3 tests/corpus/roundtrip/audit_exports.py path/to/tbcad-convert

For each DXF in tests/corpus/reader and tests/corpus/roundtrip that tbcad-convert can import, the
script converts DXF -> .tbcad -> DXF for R2000 (default), R12 and R2007 and checks that ezdxf
opens the export and its audit reports zero errors. It also checks that Thai text arrives intact
in the R2000 export, that the layer names of sub-layers use the "~" separator, and that every
DIMENSION refers to a block that exists. ezdxf (MIT) is a CI-only tool; it is never linked.
"""
import os
import subprocess
import sys
import tempfile

import ezdxf
from ezdxf.lldxf.encoding import decode_dxf_unicode

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
VERSIONS = {"r2000": "AC1015", "r12": "AC1009", "r2007": "AC1021"}


def corpus_files():
    for sub in ("reader", "roundtrip"):
        folder = os.path.join(ROOT, sub)
        for name in sorted(os.listdir(folder)):
            if name.endswith(".dxf"):
                yield os.path.join(folder, name)


def run(convert, *args):
    return subprocess.run([convert, "-q", *args], capture_output=True, text=True)


def main():
    convert = sys.argv[1]
    failures = []
    checked = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path in corpus_files():
            base = os.path.splitext(os.path.basename(path))[0]
            tbcad = os.path.join(tmp, base + ".tbcad")
            if run(convert, path, "-o", tbcad).returncode == 2:
                continue  # not importable (the broken_* files)
            for version, acadver in VERSIONS.items():
                out = os.path.join(tmp, f"{base}.{version}.dxf")
                result = run(convert, tbcad, "-o", out, "--version", version)
                if result.returncode == 2:
                    failures.append(f"{base} {version}: export failed: {result.stderr.strip()}")
                    continue
                try:
                    doc = ezdxf.readfile(out)
                except Exception as e:  # noqa: BLE001 - report whatever ezdxf says
                    failures.append(f"{base} {version}: ezdxf cannot read the export: {e}")
                    continue
                if doc.dxfversion != acadver:
                    failures.append(f"{base} {version}: version {doc.dxfversion}, wanted {acadver}")
                auditor = doc.audit()
                for error in auditor.errors:
                    failures.append(f"{base} {version}: audit error: {error.message}")
                if version != "r12":
                    handles = [e.dxf.handle for e in doc.entitydb.values()]
                    if len(handles) != len(set(handles)):
                        failures.append(f"{base} {version}: duplicate handles")
                    for e in doc.entitydb.values():
                        owner = e.dxf.get("owner")
                        if owner not in (None, "0") and owner not in doc.entitydb:
                            failures.append(f"{base} {version}: {e.dxftype()} has missing owner {owner}")
                    layers = {l.dxf.name.lower() for l in doc.layers}
                    ltypes = {l.dxf.name.lower() for l in doc.linetypes}
                    styles = {s.dxf.name.lower() for s in doc.styles}
                    for e in list(doc.modelspace()) + [x for b in doc.blocks for x in b]:
                        if e.dxf.get("layer", "0").lower() not in layers:
                            failures.append(f"{base} {version}: {e.dxftype()} on unknown layer {e.dxf.layer}")
                        lt = e.dxf.get("linetype") if e.dxf.is_supported("linetype") else None
                        if lt and lt.lower() not in ltypes:
                            failures.append(f"{base} {version}: {e.dxftype()} uses unknown linetype {lt}")
                        st = e.dxf.get("style") if e.dxftype() in ("TEXT", "MTEXT") else None
                        if st and st.lower() not in styles:
                            failures.append(f"{base} {version}: {e.dxftype()} uses unknown text style {st}")
                    for dim in doc.modelspace().query("DIMENSION"):
                        if dim.dxf.geometry not in doc.blocks:
                            failures.append(f"{base} {version}: dimension block {dim.dxf.geometry} missing")
                checked += 1
            if base == "rt_thai_r2018":
                doc = ezdxf.readfile(os.path.join(tmp, f"{base}.r2000.dxf"))
                # R2000 files are ASCII; every other character is written as \U+XXXX.
                texts = [decode_dxf_unicode(e.dxf.text) for e in doc.modelspace().query("TEXT")]
                if "สวัสดี" not in texts:
                    failures.append(f"{base}: Thai TEXT did not survive R2000: {texts}")
                names = [decode_dxf_unicode(l.dxf.name) for l in doc.layers]
                if "ห้อง~นอน" not in names:
                    failures.append(f"{base}: Thai layer name did not survive R2000")
        # Sub-layer names: import with --split-layers, export, expect the flat names back.
        src = os.path.join(ROOT, "reader", "layers_tilde.dxf")
        tbcad = os.path.join(tmp, "split.tbcad")
        out = os.path.join(tmp, "split.dxf")
        run(convert, src, "-o", tbcad, "--split-layers", "~")
        run(convert, tbcad, "-o", out)
        original = {l.dxf.name for l in ezdxf.readfile(src).layers}
        exported = {l.dxf.name for l in ezdxf.readfile(out).layers}
        if original - exported:
            failures.append(f"sub-layer names changed: lost {sorted(original - exported)}")
    print(f"{checked} exports audited")
    for f in failures:
        print("FAIL", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
