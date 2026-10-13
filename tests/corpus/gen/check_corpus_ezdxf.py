#!/usr/bin/env python3
"""Cross-checks tests/corpus/reader against ezdxf (MIT, CI and dev only).

For every corpus file that ezdxf can read (expected.json has no "ezdxf": false):
  * the file loads and passes ezdxf's audit without errors,
  * the number of entities per DXF type (model space + block definitions, layout blocks
    excluded) matches the "dxf" counts the generator wrote,
  * the layer names match "layers" (when the file is not read with --split-layers).
This proves that the expected files describe the DXF content independently of the TBCAD reader.

    pip install ezdxf && python3 tests/corpus/gen/check_corpus_ezdxf.py
"""
import collections
import glob
import json
import os
import sys

import ezdxf

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "..", "reader")


def type_counts(doc):
    counts = collections.Counter()

    def add(entities):
        for e in entities:
            counts[e.dxftype()] += 1
            if e.dxftype() == "INSERT":
                counts["ATTRIB"] += len(e.attribs)

    add(doc.modelspace())
    for block in doc.blocks:
        name = block.name.lower()
        if name.startswith("*model_space") or name.startswith("*paper_space"):
            continue
        add(block)
    return counts


def main():
    failures = 0
    checked = 0
    for path in sorted(glob.glob(os.path.join(CORPUS, "*.expected.json"))):
        base = os.path.basename(path)[: -len(".expected.json")]
        exp = json.load(open(path, encoding="utf-8"))
        if exp.get("ezdxf") is False or exp.get("exit") == 2:
            continue
        dxf = os.path.join(CORPUS, base + ".dxf")
        try:
            doc = ezdxf.readfile(dxf)
            auditor = doc.audit()
        except Exception as exc:  # noqa: BLE001
            print(f"FAIL {base}: ezdxf cannot read it: {exc}")
            failures += 1
            continue
        checked += 1
        if auditor.errors:
            print(f"FAIL {base}: audit errors: {[str(e.message) for e in auditor.errors][:3]}")
            failures += 1
        counts = type_counts(doc)
        if "dxf" in exp:
            want = collections.Counter(exp["dxf"])
            if counts != want:
                print(f"FAIL {base}: entity counts differ\n   ezdxf   {dict(counts)}\n   expected {dict(want)}")
                failures += 1
        for t, n in exp.get("dxf_min", {}).items():
            if counts[t] < n:
                print(f"FAIL {base}: {t} {counts[t]} < {n}")
                failures += 1
        if "layers" in exp and "options" not in exp and "A/child" not in " ".join(exp["layers"]):
            names = sorted(l.dxf.name for l in doc.layers)
            if names != exp["layers"]:
                print(f"FAIL {base}: layers {names} != {exp['layers']}")
                failures += 1
    print(f"{checked} files checked, {failures} problems")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
