# DWG import

`libs/dwg` reads `.dwg` files written by R2000 (AC1015) programs. It is a clean-room reader written
from the public ODA specification (see `docs/dwg-cleanroom-log.md`); no other DWG library is used.

## How it works

1. `container/r2000` reads the file header, the class table and the object map.
2. `bits/bit_reader` reads the bit-packed data types; every read is bounds-checked, so damaged data
   sets a failed flag instead of reading outside the buffer.
3. `objects/decode` turns table records and entities into plain structs (layers, linetypes, text
   styles, dimension styles, blocks, LINE, CIRCLE, ARC, POINT, ELLIPSE, SPLINE, SOLID, TRACE, TEXT,
   MTEXT, INSERT and MINSERT with their attributes, ATTDEF, 2D POLYLINE with its vertices, LWPOLYLINE,
   HATCH, the seven DIMENSION kinds with their drawn blocks, and LEADER). Other object types are
   counted and reported as skipped.
4. `map/emit` writes those structs as DXF text in memory and `dxf::importDxf` builds the document, so
   DWG and DXF share one importer and one report.

Other versions (R2004 and newer) are refused with `dwg.reader.unsupported_version`; they come with the
later container work. `tbcad-convert x.dwg out.tbcad|out.dxf` uses the same reader.

## Status and limits

Tests use files built by a small writer in `libs/dwg/tests`, so they prove the reader agrees with the
specification as read, not with files from other programs. A few details are assumptions until real
files are tried (see the last row of the clean-room log). A libFuzzer target (`TBCAD_DWG_FUZZ=ON`)
runs in CI on every change to `libs/dwg`.
