#!/usr/bin/env python3
"""Writes a large synthetic DXF for timing tbcad-convert (not committed to the corpus).

    python3 tests/corpus/gen/gen_big_dxf.py /tmp/big.dxf 50      # about 50 MB
    time build/<preset>/apps/tbcad-convert/tbcad-convert /tmp/big.dxf -o /tmp/big.tbcad
"""
import sys


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "big.dxf"
    megabytes = float(sys.argv[2]) if len(sys.argv) > 2 else 50.0
    target = int(megabytes * 1024 * 1024)
    out = ["  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1015\n  9\n$INSUNITS\n 70\n4\n  0\nENDSEC\n"
           "  0\nSECTION\n  2\nENTITIES\n"]
    size = len(out[0])
    i = 0
    while size < target:
        x, y = (i % 1000) * 10.0, (i // 1000) * 10.0
        k = i % 4
        if k == 0:
            s = f"  0\nLINE\n  8\n0\n 10\n{x:.6f}\n 20\n{y:.6f}\n 30\n0.0\n 11\n{x + 8:.6f}\n 21\n{y + 3:.6f}\n 31\n0.0\n"
        elif k == 1:
            s = f"  0\nCIRCLE\n  8\n0\n 10\n{x:.6f}\n 20\n{y:.6f}\n 30\n0.0\n 40\n2.5\n"
        elif k == 2:
            pts = "".join(f" 10\n{x + j:.6f}\n 20\n{y + (j % 2):.6f}\n" for j in range(5))
            s = f"  0\nLWPOLYLINE\n  8\n0\n 90\n5\n 70\n1\n{pts}"
        else:
            s = f"  0\nTEXT\n  8\n0\n 10\n{x:.6f}\n 20\n{y:.6f}\n 30\n0.0\n 40\n2.5\n  1\nSample label\n"
        out.append(s)
        size += len(s)
        i += 1
    out.append("  0\nENDSEC\n  0\nEOF\n")
    with open(path, "w", newline="\n") as f:
        f.write("".join(out))
    print(f"{path}: {size / 1024 / 1024:.1f} MB, {i} entities")


if __name__ == "__main__":
    main()
