# DXF import and tbcad-convert

Module `libs/dxf` (reader part, workstream W3-2) and the command line program
`apps/tbcad-convert`. The reader turns the bytes of a DXF file into a TBCAD `Document`; the
program writes that document as a `.tbcad` file. Text decoding, the tokenizer and the diagnostics
report come from W1-4 (`lexer.h`, `encoding.h`, `report.h`).

## API

```cpp
#include "tbcad/dxf/reader.h"

dxf::Report report;
dxf::ImportOptions options;                 // all fields optional
auto result = dxf::importDxf(bytes, options, report);
if (!result) { /* not a DXF file, or older than R12 */ }
Document& doc = *result.value().document;   // one undo-free "import dxf" transaction
const dxf::ImportSummary& s = result.value().summary;
std::string json = s.toJson(report);        // what --report writes
```

`importDxf` fails only when the input cannot be a DXF file or is older than AC1009 (R12).
Everything else (bad numbers, unknown entities, missing tables, truncated files) produces a
document plus diagnostics. Diagnostics with severity Info never count as warnings.

`ImportOptions`: `encoding` (force a text encoding), `insunits` (force the meaning of the drawing
units, a `$INSUNITS` code), `splitLayers` (separator for `--split-layers`), `dimensionGraphics`
(see Dimensions).

## What is read

| DXF | Result |
|---|---|
| HEADER | `$ACADVER`, `$DWGCODEPAGE` (encoding), `$INSUNITS` (units), `$LTSCALE`, `$CLAYER`; every other variable that holds no handle is kept in `Header::custom` (`dxf.hv.<name>`) and written back |
| TABLES | LAYER, LTYPE (dashes only), STYLE (an `.shx` font file becomes the TBCAD Stroke font, with an info message in the report), DIMSTYLE (text height, arrow size, gaps, scale, decimals); VPORT, VIEW, UCS, APPID, BLOCK_RECORD are ignored |
| Handles | the handle of each entity (group 5) is kept in `Header::custom` (`dxf.handles`, delta coded, about 6 bytes per entity) and given back by the DXF writer; entities made since get new handles above the highest old one, and tables, blocks and objects take the numbers that are left free. Two entities with one handle: the first keeps it. Records the reader merges or splits (attributes into their insert, a MINSERT array) keep the handle of the first entity |
| BLOCKS | `BlockDef` (name, base point, entities); `*Model_Space` and `*Paper_Space*` are skipped; anonymous blocks (`*D1`, `*U2`) are kept with `anonymous = true`; a block used before it is defined works |
| ENTITIES | see below; paper space entities (group 67 = 1) are skipped and counted |
| CLASSES | kept in `Header::custom` (`dxf.classes`) and written back |
| OBJECTS | LAYOUT, IMAGEDEF, DICTIONARY (for the lookups below) and the annotation-scale note are read. Every other object that hangs off the root dictionary under a name the writer does not make itself (materials, visual styles, multileader and table styles, page setups, application dictionaries and data) is kept in `Header::custom` (`dxf.objects`, `dxf.objroot`) with its handles replaced by tokens, and written back with new handles; a pointer to something that is not kept (an entity, a table record, a layout) becomes the null handle. Reactors and extension dictionaries of these objects are not kept |
| LAYOUT page setup | the plot settings of a layout (device, paper, plot style table, scale, origin: its `AcDbPlotSettings` part) are kept in `Header::custom` (`dxf.plotset.<layout name>`) and written back with the layout while the sheet's paper size is the same; a different paper size writes a fresh setup. Page setup changes made in TBCAD are not yet merged into the kept groups |
| THUMBNAILIMAGE | skipped |

| Entity | Result |
|---|---|
| LINE, CIRCLE, ARC, POINT | `LineData`, `CircleData`, `ArcData`, `PointData`; an ARC whose angles differ by 360 is a circle |
| LWPOLYLINE, POLYLINE (2D) | `PolylineData` with bulges, widths, closed flag; spline-fit polylines use the fitted vertices; 3D polylines are drawn flat (Z dropped, reported); polygon meshes become polylines along their rows and columns (the fitted grid of a smoothed surface) and polyface meshes become lines along their visible edges, each shared edge once (one info line `dxf.reader.flattened_3d`) |
| ELLIPSE | `EllipseData` (a ratio above 1 swaps the axes) |
| SPLINE | `SplineData`; fit-point-only splines are interpolated with `geom::interpolateSpline`, the fit points stay in `fit`; a knot vector that does not fit is replaced by a uniform one with a warning; too few control points for the degree lowers the degree (with a warning) instead of dropping the spline |
| TEXT, ATTRIB | `TextData` (`position` = group 10, `alignPoint` = group 11 when the alignment is not left/baseline, else equal to `position`); `%%c %%d %%p` are replaced; ATTDEF becomes a text with attribute-definition info (`TextData::attdef`: tag upper-cased, prompt, default, flags in group 70: 1 invisible, 2 constant, 4 verify, 8 preset) and shows its tag; the ATTRIB records after an INSERT (group 66 = 1, up to SEQEND) become that insert's `attribs` when the block has a matching non-constant definition, so the position and style of the definition win; any other ATTRIB stays a plain text (invisible ones are dropped) |
| MTEXT | `MTextData` with TBCAD markup, see below |
| INSERT | `InsertData`; MINSERT arrays are expanded into one insert per cell (up to 100 000) |
| HATCH | `HatchData`: polyline boundaries with bulges, line/arc/ellipse/spline edges, islands (`outer = false` for non-external paths), pattern name, scale and angle; the pattern definition lines are not kept; gradient fills become solid fills (info) |
| SOLID, TRACE | solid `HatchData` with one closed polyline loop (corner order 1, 2, 4, 3 as DXF stores it) |
| DIMENSION | see below |
| LEADER | `LeaderData` (vertices, dimension style); the annotation is a separate MTEXT/TEXT entity in the file and is imported as such |
| ACAD_PROXY_ENTITY | read-only `ProxyData` (`originalType = "ACAD_PROXY_ENTITY"`, `raw` = the 310 data bytes, without text records); the proxy graphics are decoded into its display list (see "Proxy graphics"); always a warning |
| MESH (subdivision mesh) | read-only `ProxyData` kept for saving, drawn from the edges of its control cage seen from above (edge records, else face boundaries, each edge once); smoothing levels are not applied |
| OLE2FRAME (pasted spreadsheets, documents) | read-only `ProxyData` kept for saving; the embedded object is not drawn, its rectangle (10/20 to 11/21) is |
| AEC_*, AECC_*, 3DSOLID, REGION, SURFACE ... (no reader) | read-only `ProxyData` named after the DXF type when the object carries proxy graphics (groups 160/92 + 310), else skipped as before; one `dxf.reader.proxy_graphics` info per type |
| MLINE | one polyline per style element (offset along the miter); closed if the closed flag is set |
| IMAGE (+ IMAGEDEF) | a picture block like IMAGEATTACH makes (docs/underlay.md); fade and a rectangle clip are kept, a polygon clip shows the whole picture |
| PDFUNDERLAY (+ PDFDEFINITION) | a PDF page block like PDFATTACH makes ("file.pdf#page=N,fade=F"); placement, scale, rotation, page, fade, contrast and monochrome are kept, the clip boundary is not. PDF page blocks are written back as PDFUNDERLAY + PDFDEFINITION |
| TOLERANCE | a feature control frame block `TB_TOL_n` like the TOLERANCE command makes (docs/annotative.md): symbol, value, diameter, material condition and up to three datums are read from the gdt-font text, the size from the named dimension style (text height x overall scale), and the rotation from the direction vector; text that is not a frame stays a placeholder |
| everything else (3DSOLID, REGION, SURFACE, ...) | a read-only `ProxyData` that holds the record's groups (`ProxyData::raw`, see `kProxyDxfFresh`) and the proxy graphics when there are any. It is not shown when it has no graphics. Saving as DXF writes the record back as it was read (layer, colour and the other common properties come from the document; handles and extension dictionaries are not kept, XDATA is). Moving, rotating or scaling it marks the groups stale and only the graphics are written. Skipped, with one warning per type, when the record has nothing but common properties or the drawing's units were scaled |

`ImportSummary::entities` counts, per DXF type, the records read (model space and block
definitions), the TBCAD entities produced and the records dropped.

## Coordinates

* All geometry is converted to millimetres and 2D; a non-zero Z is dropped (one info line with the
  number of such entities).
* **Extrusion / OCS.** Entities whose extrusion is (0, 0, 1) are used as they are. Extrusion
  (0, 0, -1) means the entity's x axis points along world -X: points are mirrored about the Y axis,
  an arc `a..b` becomes `pi-b .. pi-a` (start and end swap), bulges and hatch arc directions
  flip, polyline/hatch coordinates are mirrored, the rotation of text is `pi - r`, an INSERT gets
  `rotation = -r` and `scale.x = -sx`, an ELLIPSE keeps its centre and axis (they are in world
  coordinates) but its parameters run the other way. Text and MText can only be placed correctly,
  not mirrored (one info line `dxf.reader.mirrored_text`). A normal within a few degrees of Z is
  treated as flat, and one with |nz| >= 0.9 goes through the arbitrary axis algorithm. A plane
  tilted further is drawn as seen from above: circles and arcs become ellipses (or a straight
  polyline when the plane is vertical), an ELLIPSE gets its projected axes, and an LWPOLYLINE is
  projected with its bulges cut into short segments, as is a 2D POLYLINE. HATCH, SOLID and TRACE
  are built in the object plane and projected (arc and elliptical boundary edges become short
  lines; spline edges keep their shape). One info line `dxf.reader.extrusion_3d` counts them.
  TEXT, ATTDEF and ATTRIB (also the loose attributes of an insert) keep their exact projected shape (rotation, height, width factor,
  obliquing angle, upside down); MTEXT gets the projected height and width. Text seen edge-on is
  skipped as a bad entity. An INSERT turned by a multiple of 90 degrees in its plane becomes a
  block squashed by the tilt (rotation plus non-uniform scale); any other turn would need a shear
  and is skipped with a warning of the same code, as are other entity types in such a plane.
* **Units.** `$INSUNITS` is converted with the usual factors (inch 25.4, foot 304.8, metre 1000,
  ...). Unitless (0) or missing: the numbers are taken as millimetres and an info line says so.
  When a factor other than 1 is applied an info line names it. Every length is scaled: coordinates,
  radii, text heights, widths, linetype dashes, dimension sizes and measurements, block base
  points. `--units` / `ImportOptions::insunits` overrides the file.
* **Limits.** Geometry beyond the hard cap (1e10 mm) is dropped with `dxf.reader.beyond_hard_cap`;
  geometry beyond the working area (1e8 mm) is kept and one warning counts it (the document also
  flags it in its change set).

## Colour, line weight, linetype, visibility

* ACI 1..7 keep their meaning; ACI 8..255 map to the nearest entry of the TBCAD palette (squared
  RGB distance; the ACI layout is rebuilt from its public rule: grays, 24 hues x 10 shades).
  256 = ByLayer, 0 = ByBlock, negative = the absolute value. True colour (group 420) wins.
* Line weight (group 370) in hundredths of a millimetre maps directly; -1 ByLayer, -2 ByBlock,
  -3 Default.
* Linetype names are looked up in the imported LTYPE table; `BYLAYER`, `BYBLOCK`, `CONTINUOUS` map
  to the symbolic ids; an unknown name warns once and falls back to ByLayer.
* Group 60 = 1 sets `visible = false`. Layer off (negative colour), frozen (70 bit 0), locked
  (70 bit 2) and plot (290) are kept on the layer.

## Levels

The `TBCAD` APPID record's XDATA (`levels-current`, `levels-view`, `levels-row` + elevation + flags)
recreates the level table, empty levels included, and each entity's `1000 level=<name>` puts it back
on its level (see `dxf-export.md`). Without that XDATA, which is every file from another program,
everything lies on the first level.

## Layers and sub-layers

DXF layers are flat. Three ways a hierarchy appears:

1. **TBCAD XDATA** (written by the exporter, W4-6): a LAYER row that carries XDATA for APPID
   `TBCAD`:
   ```
   1001 TBCAD
   1000 parent=<DXF name of the parent layer>
   1000 name=<own short name>
   1070 <flags>    bit 0 colour is ByParent, bit 1 linetype is ByParent, bit 2 lineweight is ByParent
   ```
   The tree is rebuilt exactly, whatever the order of the rows. A missing or unusable parent gives
   `dxf.reader.sublayer_parent_missing` and a top-level layer with the full DXF name.
2. **`--split-layers <sep>`** (`ImportOptions::splitLayers`): names like `S-FLOOR~edge` become
   layer `edge` below `S-FLOOR`; parents that are not DXF layers are created with default
   properties; more than 8 levels join the rest into the last name. The leaf keeps its own colour,
   linetype and lineweight.
3. Otherwise layers stay flat.

A layer used by an entity but missing from the table is created with default properties
(`dxf.reader.missing_layer`).

## MTEXT markup

DXF MTEXT strings are decoded (`\U+XXXX`, `\M+`, the file's encoding) and converted to the TBCAD
markup of `docs/text.md` (W4-4):

* kept: `\P` becomes a newline; `\\ \{ \}`; bold/italic taken from `\f...|b1|i1;` as `{\b1;..}`
  `{\i1;..}`; `\H<h>;` as `\H<h in mm>;` (`\H2x;` stays relative); `\C<aci>;` as `\C<index>;`
  with the palette mapping; `\S1/2;` stacked fractions (also `#` and `^` forms) are kept;
* flattened: `\~` becomes a space, `%%c`/`%%d`/`%%p` their characters;
* dropped and counted in one info line: `\A \Q \T \W \X \p \L \l \O \o \K \k \N` and the font
  name.

## Dimensions

`DIMENSION` becomes `DimensionData` (kind, definition points, text position and override, style,
measurement; when the file has no measurement it is derived from the points). `defPts` holds, per
kind:

| kind | `defPts[0..3]` |
|---|---|
| Linear, Aligned | group 13, group 14, group 10 (point on the dimension line), unused |
| Radius, Diameter | group 10 (centre / first point), group 15 (point on the circle), unused, unused |
| Angular2Line | 13, 14, 15, 10; the point on the arc (group 16) goes to `textPos` |
| Angular3Point | 13, 14, 15 (vertex), 10 (arc point) |

Some writers (ezdxf, some CAD programs) store an aligned dimension as a rotated linear one;
that is imported as Linear with `rotation`. Ordinate dimensions have no `DimensionData` form.

Dimensions draw themselves from their definition points and style (`docs/dim.md`), so the file's
own drawing (the anonymous `*D` block) is not needed. After the import the text position of every
dimension whose text the user did not move is recomputed by the generator, which makes
export, import and export give the same dimension again; the stored measurement is kept. With
`dimensionGraphics` on (converter `--dimension-graphics`) the block is also kept as one
`ProxyData` entity (`originalType = "DIMENSION"`, resolved `DisplayList`) and loose
`TextData`/`MTextData`, which shows exactly what the other program drew. Off by default.

## Report and exit codes

`ImportSummary::toJson` / `--report`:

```json
{"input":{"acadver":"AC1015","version":"R2000","binary":false,"encoding":"ANSI_874","encodingGuessed":false},
 "units":{"insunits":4,"name":"millimeters","scaleToMm":1,"assumed":false},
 "entities":{"LINE":{"read":10,"imported":10,"skipped":0}},
 "summary":{"layers":3,"blocks":1,"outsideWorkArea":0,"beyondHardCap":0},
 "report":{"info":1,"warnings":0,"errors":0,"diagnostics":[{"severity":"info","code":"...","message":"...","line":0}]}}
```

Diagnostic codes: those of the lexer (`dxf.lexer.*`), the encoding detection (`dxf.encoding.*`) and
the reader (`dxf.reader.*`, constants in `reader.h`). Line numbers are those of the DXF file (byte
offsets for binary DXF).

## tbcad-convert

```
tbcad-convert input.dxf -o output.tbcad [--report report.json] [--units mm|cm|m|km|in|ft|yd|mi|<n>]
              [--encoding utf-8|cp874|cp1252|...] [--split-layers <sep>]
              [--dimension-graphics] [-q]
```

Exit codes: **0** converted without warnings, **1** converted with warnings (printed to stderr
unless `-q`, and listed in the report), **2** failed (not a DXF file, unsupported version, cannot
read or write a file, bad command line). The `.tbcad` file is written atomically by `libs/io`. With
`--report` a report is also written for a failed run (`{"failed":true,"error":"..."}`).

## Proxy graphics

AutoCAD stores a drawing of each object it cannot rebuild (ACAD_PROXY_ENTITY, AEC walls, Civil 3D
alignments, ACIS solids) as a binary record stream: an 8-byte header (total size, record count) and
records `size, type, payload`. `libs/dxf/src/reader/proxy.cpp` reads circles, arcs, ellipse arcs,
polylines, polygons, shells, meshes, triangles, text (ANSI in the file codepage and Unicode),
colour (ACI and true colour), lineweight, fill and matrix records. It is bounds checked: a
record with an impossible size ends decoding (`dxf.reader.proxy_graphics_damaged`), a record of
an unknown kind is counted in `dxf.reader.proxy_graphics_unknown_records`, and counts, point numbers and
non-finite values are limited. The result is shown as a read-only `Proxy` entity; its texts become
ordinary Text entities (and are removed from `raw`, so exporting does not draw them twice). The
DXF exporter writes an ACAD_PROXY_ENTITY back unchanged and writes other foreign objects as the
plain geometry of their graphics.

## Hostile files

The corpus also covers: invalid UTF-8 in R2018 files, cp874 files without `$DWGCODEPAGE`, NaN,
infinite, comma-decimal and empty numbers, broken and duplicate handles, tables that are missing
(layer, linetype, style, block record) and files without HEADER or TABLES, swapped and unclosed
sections. Each is reported and imported as far as it can be.

## Test corpus

`tests/corpus/reader/` holds about 80 DXF files made by `tests/corpus/gen/gen_corpus.py` (ezdxf) and `gen_hardening.py` (plain Python: proxy graphics, AEC objects, bad encodings, hostile numbers, broken handles, missing tables) (R12,
R2000, R2004, R2007/R2013/R2018, Thai in cp874 and UTF-8, binary, extrusion -Z, far coordinates,
proxy, unknown entities, sub-layers, MTEXT markup, hatch variants, dimensions and several broken
files), each with an `.expected.json`. `libs/dxf/tests/reader/corpus_tests.cpp` checks entity counts
per DXF type, model space entities per kind, layer paths, texts, the warning codes and the exit
status. `tests/corpus/gen/check_corpus_ezdxf.py` cross-checks the expected counts with ezdxf in CI.
Robustness: `mutation_tests.cpp` (2500 mutated files in every test run, also under ASan/UBSan) and
the libFuzzer target `fuzz_reader.cpp` (`.github/workflows/fuzz-dxf.yml`).

Speed: a 50 MB synthetic DXF (about 480 000 entities) imports in 0.65 s and converts end to end
(read, import, write `.tbcad`) in 1.4 s with a Release build on the development container.

## Not done yet

OBJECTS (layouts, groups, dictionaries), paper space and viewports, xrefs (the block stays empty),
proxy graphics records for 3D mesh/shell lighting, images and rich text frames, complex linetype elements (text/shape), gradient colours,
3D entities, mirrored text, DIMSTYLE variables beyond the basic ones.

## Codepages

Tables live in `libs/dxf/src/encoding/tables/` and are produced by `libs/dxf/tools/gen_codepages.py`:
with `--mapping-dir DIR` from the Unicode Consortium `CP*.TXT` vendor mapping files, without it from
Python's codecs, which derive from the same files (both give identical output; a CI-less offline
machine can regenerate). Codepages: 874, 932, 936, 949, 950, 1250-1258 and 1361 (Johab, `ANSI_1361`).

`\M+nXXXX` escapes: n = 1 Shift-JIS (932), 2 Big5 (950), 3 KS C 5601 (949), 4 Johab (1361),
5 GB2312 (936). `libs/dxf/tools/gen_mplus_vectors.py` writes `tests/corpus/encoding/mplus_vectors.txt`
(about 2100 sample escapes and the characters the platform codecs give for them), and the encoding
tests check `decodeText` against it.

## MULTILEADER, 3DFACE, WIPEOUT and ARC_DIMENSION

A MULTILEADER comes in as one multileader object: its leader lines, the landing point, the dogleg to the text and the text; block content and the leader style's appearance are not read. A 3DFACE comes in as its visible edges projected on the XY plane. A WIPEOUT becomes a closed polyline that hides what is behind it, and an ARC_DIMENSION an arc length dimension.
