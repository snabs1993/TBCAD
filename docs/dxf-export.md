# DXF export and round trip

Module `libs/dxf` (writer part, workstream W4-6) and the `.tbcad -> DXF` half of
`apps/tbcad-convert`. It is the reverse of the reader described in `dxf-import.md`.

## API

```cpp
#include "tbcad/dxf/writer.h"

dxf::Report report;
dxf::ExportOptions options;                  // all fields optional
options.version = dxf::DxfVersion::R2000;    // R12, R2000 (default), R2007
options.layerSeparator = "~";                // default
auto result = dxf::exportDxf(doc, options, report);
if (result) {
    std::string& bytes = result.value().bytes;              // the complete file
    std::string json = result.value().summary.toJson(report);  // what --report writes
}
```

`exportDxf` fails only for an unusable layer separator (empty, `|`, `$` or a character DXF
forbids in names). Everything else produces a file plus diagnostics (`dxf.writer.*`, constants in
`writer.h`). Text is always valid for the chosen version; the exporter never writes a name that
other programs reject.

## Versions

| Option | `$ACADVER` | Text | Notes |
|---|---|---|---|
| `--version r2000` (default) | AC1015 | ASCII; every non-ASCII character is written as `\U+nnnn` (surrogate pairs above U+FFFF), `$DWGCODEPAGE` is `ANSI_1252` | accepted by every reader |
| `--version r2007` or `--utf8` | AC1021 | UTF-8 | same structure as R2000 |
| `--version r12` | AC1009 | same escaping | no handles, no BLOCK_RECORD, no OBJECTS, no 100 markers; LWPOLYLINE, ELLIPSE, SPLINE, MTEXT, HATCH and LEADER do not exist and are written in simpler form (below) |

Lines end with CRLF. Numbers are written as the shortest text that reads back as the same double
(`1e-07` style exponents appear for very small values; they are valid DXF).

## File structure (R2000 and R2007)

* **HEADER**: `$ACADVER`, `$HANDSEED` (above every handle in the file), `$DWGCODEPAGE`,
  `$INSBASE`, `$EXTMIN`/`$EXTMAX` (from model space), `$LTSCALE`, `$CLAYER`, `$TEXTSTYLE`,
  `$DIMSTYLE`, `$INSUNITS = 4` (millimetres), `$MEASUREMENT = 1`, units/precision variables.
* **CLASSES**: empty.
* **TABLES**: VPORT (`*Active`, zoomed to the extents), LTYPE (`ByBlock`, `ByLayer`,
  `Continuous` first, then the document's), LAYER, STYLE (text styles that use the TBCAD Stroke font are written with `txt.shx`, AutoCAD's own stand-in; TBCAD reads it back as the stroke font), VIEW and UCS (empty), APPID (`ACAD`,
  `TBCAD`), DIMSTYLE, BLOCK_RECORD (`*Model_Space`, `*Paper_Space`, then every block).
* **BLOCKS**: `*Model_Space`, `*Paper_Space`, the document's blocks in id order, then one
  anonymous `*D<n>` block per dimension.
* **ENTITIES**: model space in draw order.
* **OBJECTS**: root dictionary with `ACAD_GROUP`, `ACAD_LAYOUT` (the two LAYOUT objects),
  `ACAD_MLINESTYLE` (Standard) and `ACAD_PLOTSTYLENAME` (Normal).

Every object has a unique handle and a correct owner (group 330): table rows belong to their
table, entities to the BLOCK_RECORD of their block (or of `*Model_Space`), `BLOCK`/`ENDBLK` to
their record. A missing owner is the first thing AutoCAD rejects, so the test suite checks all of
it (`libs/dxf/tests/writer/writer_tests.cpp`) and CI audits real exports with ezdxf.

## Names

DXF layer, linetype, style and block names may not contain `< > / \ " : ; ? * | = ``` ` ```.
Such characters become `_`; a name that then collides (names compare case-insensitively) gets a
`_2`, `_3` suffix; R12 names are cut to 31 characters. Every change is an info diagnostic
`dxf.writer.name_changed`. Block names that begin with `*` stay anonymous blocks.

## Levels

A drawing with more than one level (or a renamed or hidden first level) writes its levels as TBCAD
XDATA, so a TBCAD to TBCAD round trip through DXF keeps them. The table is XDATA on the `TBCAD`
APPID record: `1000 levels-current=<name>`, `1000 levels-view=<0|1|2>` and per level
`1000 levels-row=<name>`, `1040 <elevation mm>`, `1070 <bit 0 visible, bit 1 locked>`. Every
entity ends with `1001 TBCAD` / `1000 level=<name>`. Other programs ignore all of it and see every
object, which is the right result for a floor-plan reader that flattens the levels. A drawing with
only the untouched first level writes nothing extra. `ExportOptions::onlyLevel` (library API)
writes just that level as a plain drawing.

## Layers and sub-layers

DXF layers are flat. A sub-layer is written under its full path joined by the separator
(`--layer-separator`, default `~`), for example `S-FLOOR~edge~deep`, and carries TBCAD XDATA that
the reader (`dxf-import.md`) understands:

```
1001 TBCAD
1000 parent=S-FLOOR~edge       DXF name of the parent layer
1000 name=deep                 the layer's own name
1070 7                         bit 0 colour, bit 1 linetype, bit 2 lineweight are ByParent
```

So the tree and the ByParent settings survive the trip through DXF, while other programs see the
effective colour, linetype and lineweight in the layer's own groups. Effective on/frozen/locked/
plot are written the same way (a child of a switched-off parent is written off); this is the one
place where the exporter loses information, and it says so with `dxf.writer.layer_state_flattened`.
Do not choose `|` (xref layers) or `$` (bound xrefs) as separator; the exporter refuses them.

## Entities

| TBCAD | DXF (R2000) | R12 |
|---|---|---|
| Line, Circle, Arc, Point | LINE, CIRCLE, ARC, POINT | same |
| Polyline | LWPOLYLINE (bulges, widths, closed) | POLYLINE + VERTEX + SEQEND |
| Ellipse | ELLIPSE (parameters normalised, full ellipse 0..2pi) | polyline, chord tolerance 0.01 mm |
| Spline | SPLINE (knots, weights, control and fit points; a missing knot vector becomes clamped uniform) | polyline |
| Text | TEXT with alignment, width factor, oblique, style | same |
| MText | MTEXT in chunks of 250 characters; TBCAD markup is converted back (`\P`, bold/italic as a `\f` font code, `\H`, `\C` as ACI) | one TEXT per line |
| Insert | INSERT with scale and rotation; one ATTRIB per non-constant attribute of the block and a SEQEND (group 66 = 1) | same; attributes read back as insert values |
| Attribute definition | ATTDEF (tag, prompt, default, flags) inside the block | same |
| Hatch | HATCH: polyline and edge paths (line, arc, ellipse, spline), islands, pattern name, scale, angle; the pattern's definition lines are embedded; gradient fills (kind, angle, two colours) | boundary outline only |
| Dimension | DIMENSION + anonymous block `*D<n>` (below) | same |
| Leader | LEADER (+ an MTEXT for its annotation) | polyline |
| Proxy | `ACAD_PROXY_ENTITY` with its data; proxies that only carry graphics become LINE/LWPOLYLINE/SOLID/HATCH/POINT | graphics only |

Colours: an index is written as the ACI the reader maps back to the same index, so DXF files
round-trip exactly; the few palette entries no ACI maps to, and RGB colours, are written as the
nearest ACI plus group 420 (true colour). Line weights are rounded to the nearest value DXF
allows (0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120, 140, 158,
200, 211 hundredths of a millimetre). Hidden entities get group 60.

### Hatch patterns

Pattern definitions come from the W5-4 library (`tbcad::hatch::PatternRegistry`: the built-in
`LINE0`, `LINE45`, `CROSS45`, `GRID`, `BRICK`, `DOTS`, `SAND`, `EARTH` and the user's own
patterns). They are embedded as definition lines (group codes 53/43/44/45/46/79/49) with the
hatch's scale and angle applied, so other programs draw them; `SOLID` needs no lines.
`ExportOptions::patterns`, when set, is asked first. A name nobody knows is written without lines
(AutoCAD draws its own `ANSI31` and friends by name) and reported as `dxf.writer.unknown_pattern`
(info). The reader keeps only the pattern name; the definition comes from the library on display.

### Dimensions

The exporter writes a `DIMENSION` entity (definition points, text position `11`, measurement `42`,
text override, style name, flag 128 when the user placed the text) and a fresh anonymous `*D<n>`
block that it references. The block is drawn by the dimension generator (`dim::explodeDimension`):
extension and dimension lines, arcs, solid arrowheads (as HATCH) and the text, so programs that do
not regenerate dimensions still show them. Group `11` is the middle of that text. Two-line angular
dimensions also write group `16`, the point on the arc; three-point ones write `13`, `14`, vertex
`15` and arc point `10`. Arc length dimensions are skipped with a warning. The `*D` blocks that
came with an imported document are never copied; `dxf.writer.dimension_graphics_missing` is no
longer reported.

## Command line

```
tbcad-convert input.tbcad -o output.dxf [--version r2000|r12|r2007] [--utf8]
              [--layer-separator SEP] [--report report.json] [-q]
```

`.tbcad` input selects this direction; any other input is imported from DXF as before. Exit codes:
**0** written, **1** written with warnings, **2** failed (unreadable input, unusable options,
cannot write). The file is written to `<output>.tmp` first and renamed.

## Tests and CI

* `libs/dxf/tests/writer/roundtrip_tests.cpp`: every file of `tests/corpus/reader` and
  `tests/corpus/roundtrip` is imported, exported, imported again and compared; a second export is
  byte for byte the first.
* `apps/tbcad-convert/tests/export_tests.cpp`: the same through the command line and `.tbcad`
  files: DXF -> tbcad -> DXF -> tbcad.
* The comparison (`libs/dxf/tests/writer/semantic_dump.h`) is the canonical dump of the document
  with ids replaced by names (ids depend on read order), dimension blocks left out (they are
  regenerated), closing points of filled rings and angles modulo a full turn treated as equal, and
  numbers compared within 1e-9 relative.
* `libs/dxf/tests/writer/writer_tests.cpp` and `entity_tests.cpp`: file structure, handles and
  owners, Thai text in R2000 and R2007, sub-layer XDATA and tree, name repair, R12, every entity
  kind, colours, patterns, dimensions, proxies.
* `tests/corpus/roundtrip/gen_roundtrip.py` makes the extra corpus files with ezdxf (CI-only
  tool); `tests/corpus/roundtrip/audit_exports.py` exports every corpus file as R2000, R12 and
  R2007 and runs the ezdxf audit (`.github/workflows/dxf-export.yml`): zero errors, unique
  handles, existing owners, layers, linetypes, styles and dimension blocks, Thai text intact.

Speed: a 50 MB DXF (about 490 000 entities) is read, converted to `.tbcad` in 1.7 s and exported
back (74 MB of DXF) in 1.4 s with a Release build.

## Not done yet

Paper space and layouts beyond the two default ones, XDATA other than TBCAD's,
hatch associativity, arc-length dimensions (skipped with a warning), dimension style variables
beyond the basic ones, binary DXF output.

## Commands

`EXPORTDXF` asks for a file name and a version (R2000 default, R12, R2007) and writes the whole
drawing. `IMPORTDXF` asks for a file name and adds the contents of the DXF file to the open
drawing in one undo step; layers, linetypes and styles are matched by name, and blocks whose name
is taken are renamed `<name>_1`. Both live in `libs/commands/dxfexport`.

## Tables

A table is written as an INSERT of an anonymous `*T<n>` block (grid lines and one MTEXT per filled cell) with
TBCAD extended data that carries the table; see `docs/table.md`.
