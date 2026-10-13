# Blocks, array, explode (`tbcad::blocks`, `block_cmds`)

`libs/blocks` (`tbcad::blocks`) holds the algorithms; `libs/commands/blocks` (target
`tbcad_block_cmds`) holds the commands and the block list panel. The folder and the target differ
because `tbcad_blocks` is already the library.

## Model

A `BlockDef` has a name, a base point and the ids of its entities (stored in block space, the
coordinates they were drawn with). An `Insert` places a block:

```
block space -> translate(position) * rotate(rotation) * scale(sx, sy) * translate(-base)
```

`blocks::instanceTransform(insert, base)` returns it. A negative scale mirrors. Blocks may contain
Inserts of other blocks; `blocks::wouldCreateCycle(doc, container, inserted)` and
`blocks::references(doc, from, target)` find cycles, and BLOCK refuses to redefine a block with a
selection that contains an Insert of itself (directly or through another block).

## Display

`blocks::registerInsertDisplay()` (called by `registerCommands`) registers the Insert display and
bounds generators with `geom::DisplayRegistry`.

* ByBlock colour, lineweight and linetype take the Insert's resolved values (an Insert that is
  itself ByBlock passes its own parent's down).
* Entities on layer 0 take the Insert's layer, both for colour resolution and for layer
  visibility. Entities on other layers keep their layer.
* The block is tessellated in block space, then transformed. Wrap a rebuild in a
  `blocks::BlockDisplayScope` (with one `BlockDisplayCache` per document) and Inserts that share a
  block, style and tolerance share one list. The cache drops its entries when the document
  revision changes. Without a scope every Insert is tessellated on its own.
* Tolerance follows the instance scale, quantised to a power of two so nearby zoom levels share.

## Explode

`blocks::explode(doc, entity)` returns new entities (null ids) and messages; EXPLODE erases the
original and adds the parts in one undo step.

* Insert: one level. Layer-0 children take the Insert's layer; ByBlock colour, linetype and
  lineweight take the Insert's. Nested Inserts stay Inserts.
* A circle or arc under non-uniform scale becomes an ellipse (via `geom::transformed`). A polyline
  with arcs is split into lines and elliptical arcs. Text, MText, dimensions and leaders are scaled
  evenly (message). A nested Insert that cannot represent the scale is flattened (message).
* Polyline: lines and arcs; widths are dropped and reported.
* Hatch is wired to `hatch::explodeHatch`. Dimension and Leader register a hook with `blocks::ExplodeRegistry::set(kind, fn)` when
  their modules exist. Kinds without a hook are not selectable by EXPLODE.

## Array

`blocks::arrayRectangular(source, RectArray)` and `arrayPolar(source, PolarArray)` return the
copies (the original is cell 0 and is not repeated). Rows go along the rotated Y axis, columns along
the rotated X axis; negative spacing goes the other way. A polar array spreads `count` items over
`fillAngle` (a full circle puts none on top of the original; a partial angle puts the last item at
the end). Without `rotateItems` the objects keep their orientation and the centre of their bounding
box follows the circle (or `basePoint`). Arrays are limited to 100,000 copies. The result is plain
copies (answer `No` to the associative question of `ARRAY`).

### Associative arrays

An associative array is one Insert whose `InsertData::array` (`ArrayInfo`) says how many times and
where the block is shown. `ARRAY` moves the selected objects into a new anonymous block (`*A1`, `*A2`,
...) with base point (0, 0) and inserts it at the origin, so the array geometry (centre, path, spacing)
is in block coordinates and moving, rotating or scaling the Insert moves the whole array. The
parameters stay editable with `ARRAYEDIT`; the source objects can be edited by changing the block.

`blocks::arrayItemTransforms(info)` gives the block-space transform of every item (item 0 included):
rectangular and polar items are translations or rotations about the centre (item 0 is the identity);
a path array puts the base point on the path start for item 0 and at `pathSpacing` multiples of arc
length for the others (0 spreads the items evenly: over the length for open paths, round the loop for
closed ones), turning each item by the change of the path direction when `rotateItems` is set.
`instanceTransforms(insert, base)` multiplies them with the instance transform; display, bounds, snap
geometry and explode all use it, so every item is drawn, measured, snapped to and exploded. Exploding
an array gives the individual objects of all items.

DXF has no associative array: export writes one INSERT per item (with its ATTRIBs). A stretched Insert
(unequal X and Y scale) whose items turn is written with the nearest placement and a note in the export
report. In `.tbcad` the array is a trailing field of the Insert record (format 1.3).

## Commands

| Command | Alias | Flow |
| --- | --- | --- |
| `BLOCK` | `B` | name, base point, objects, then `Retain/Convert/Delete`. An existing name offers `Redefine`. |
| `INSERT` | `I` | block name (`?` lists, Enter takes the last one), then the point with `Scale/X/Y/Rotate` options and a live preview. |
| `EXPLODE` | `X` | objects (selected inserts and polylines), one level. |
| `ARRAY` | `AR` | objects, `Rectangular` (rows, columns, column distance, row distance, angle), `Polar` (centre, count, angle to fill, rotate items) or `Path` (path curve, base point, count or distance, align), with preview, then `Associative [Yes/No]` (default Yes; a path array is always associative). |
| `ARRAYRECT` / `ARRAYPOLAR` / `ARRAYPATH` | | The same, for one type. |
| `ARRAYEDIT` | | Pick an associative array and change its rows, columns and spacing (rectangular), or its count and angle or distance (polar, path). One undo step. |

Defaults are 3 x 4 for rectangular arrays (spacing 1.5 x the selection's size) and 6 items over
360 degrees for polar ones, so pressing Enter all the way gives a sensible result.

## Object snap

`blocks::snapGeometry(doc, insert)` returns the curve and point geometry of an Insert flattened
through nested blocks and placed in the Insert's space. `registerCommands` sets it up as the Insert
entry of `drafting::SnapGeometryRegistry::instance()` (`block_cmds::registerSnapProvider`): each
part offers what a stand-alone entity of its kind would (endpoints, midpoints, centres, quadrants,
curves for intersection, perpendicular and tangent).

## Spatial index

An Insert's box depends on its block, which `index::DocumentIndexer` cannot see.
`blocks::BlockIndexRefresher(doc, indexer)` is an observer that reindexes every model-space Insert
whose block (or a block nested in it) was touched by a commit, undo or redo. Create it right after
the `DocumentIndexer`.

## Block list panel

`Blocks` dockable panel (hidden by default): filter box and a table of name, object count and
number of uses; double-click inserts. The rows come from `block_cmds::blockRows`, which is tested
without a window. The ImGui part is compiled only when `TBCAD_BUILD_APP` is on.

## Wiring in the application

`AppCore` creates a `BlockIndexRefresher` right after each session's `DocumentIndexer`, and
`SceneSync` owns a `BlockDisplayCache` and opens a `BlockDisplayScope` around every display build,
so Inserts of one block are tessellated once per document revision. Measured in a Debug build on a
Linux container: 10,000 inserts of a 21-entity block, display lists alone 0.81 s without the cache
and 0.18 s with it (one block-space build); `SceneSync::rebuildAll` including the scene upload
1.9 s. The test `perf: 10,000 inserts ...` in `libs/ui/tests/block_scene_tests.cpp` prints the
numbers and checks the cache is faster.

## Attributes

An attribute definition is a `Text` inside a block with `TextData::attdef` set (`AttDefInfo`: tag,
prompt, default value, and the Invisible / Constant / Verify / Preset flags). Each Insert carries
its own values in `InsertData::attribs` (tag + value). No new entity kind is involved, so older
readers see an ATTDEF as plain text showing its tag.

* `ATTDEF` (`ATT`): mode keywords (`Invisible`, `Constant`, `Verify`, `Preset`, `Done`), tag
  (upper-cased, no spaces), prompt, default value, point, height, rotation.
* `BLOCK` keeps the definitions, refuses two with the same tag, gives a converted insert the
  default values, and when it redefines a block it updates that block's inserts (`syncInserts`).
* `INSERT` asks for each attribute that is not constant or preset (twice for Verify). Setting
  `block.attreq` = `0` skips the questions and keeps the defaults.
* `ATTEDIT` (`ATE`) changes the values of one insert; `ATTSYNC` brings every insert of a block in
  line with its current definitions (new tags get the default, removed tags are dropped).
* Display: the block's shared display cache leaves definitions out; `buildInsertDisplay` draws
  each visible attribute with the insert's value, on the definition's own layer rules. Bounds
  include the attribute text. `EXPLODE` turns attributes into plain text with the values
  (invisible ones disappear).
* `.tbcad` version 1.1 stores the extra fields (see `docs/file-format.md`); DXF: ATTDEF and INSERT + ATTRIB + SEQEND
  map to definitions and insert values (see `docs/dxf-import.md`, `docs/dxf-export.md`). An exported
  ATTRIB sits where the definition lands after the insert transform; text is never stretched, so
  its height follows the Y scale.

## External references

An xref is a block whose content is a snapshot of another drawing; see `docs/xref.md`.

## Block palette, library and block count

The `Blocks` panel is a palette with two tabs.

* **Drawing**: one tile per named block with a line thumbnail (`thumbnailOfBlock`), the number of
  uses (or `xref`), a filter box and a `Count` button. Double-click inserts; right-click offers
  Insert and, for xrefs, Reload / Bind / Detach.
* **Library**: a folder of `.tbcad` and `.dxf` files (setting `block.library`, three levels deep,
  at most 2,000 files). Thumbnails load one file per frame. Double-click inserts the file as a
  block; right-click can attach it as an xref instead.

`INSERT` accepts the name of a library file in place of a block name (absolute path, or relative
to the drawing's folder or the library folder; `.tbcad` may be left out). The file becomes an
ordinary block named like the file (`xref::importAsBlock`: layers are matched by name, missing
ones are created, layer 0 stays layer 0, nested blocks are copied as `name$block`) the first time;
later inserts reuse it. Esc during the insert leaves nothing behind.

`BLOCKCOUNT` (`BCOUNT`) lists each used block with the inserts placed in model space and the total
including blocks nested in other inserted blocks, then offers `Table` (a grid of text and lines at
a point), `Csv` (a file with `Block,Count,Total`) or `None`.

The ImGui part of the palette is compiled only with `TBCAD_BUILD_APP`; everything it shows comes
from `palette.h`, which the tests cover.
