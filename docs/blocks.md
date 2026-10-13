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
* Dimension, Hatch and Leader register a hook with `blocks::ExplodeRegistry::set(kind, fn)` when
  their modules exist. Kinds without a hook are not selectable by EXPLODE.

## Array

`blocks::arrayRectangular(source, RectArray)` and `arrayPolar(source, PolarArray)` return the
copies (the original is cell 0 and is not repeated). Rows go along the rotated Y axis, columns along
the rotated X axis; negative spacing goes the other way. A polar array spreads `count` items over
`fillAngle` (a full circle puts none on top of the original; a partial angle puts the last item at
the end). Without `rotateItems` the objects keep their orientation and the centre of their bounding
box follows the circle (or `basePoint`). Arrays are limited to 100,000 copies. The result is plain
copies; there is no associative array in v1.

## Commands

| Command | Alias | Flow |
| --- | --- | --- |
| `BLOCK` | `B` | name, base point, objects, then `Retain/Convert/Delete`. An existing name offers `Redefine`. |
| `INSERT` | `I` | block name (`?` lists, Enter takes the last one), then the point with `Scale/X/Y/Rotate` options and a live preview. |
| `EXPLODE` | `X` | objects (selected inserts and polylines), one level. |
| `ARRAY` | `AR` | objects, `Rectangular` (rows, columns, column distance, row distance, angle) or `Polar` (centre, count, angle to fill, rotate items), with preview. |

Defaults are 3 x 4 for rectangular arrays (spacing 1.5 x the selection's size) and 6 items over
360 degrees for polar ones, so pressing Enter all the way gives a sensible result.

## Object snap

`blocks::snapGeometry(doc, insert)` returns the curve and point geometry of an Insert flattened
through nested blocks and placed in the Insert's space. The drafting aids use it as the Insert
entry of their `SnapGeometryProvider`.

## Spatial index

An Insert's box depends on its block, which `index::DocumentIndexer` cannot see.
`blocks::BlockIndexRefresher(doc, indexer)` is an observer that reindexes every model-space Insert
whose block (or a block nested in it) was touched by a commit, undo or redo. Create it right after
the `DocumentIndexer`.

## Block list panel

`Blocks` dockable panel (hidden by default): filter box and a table of name, object count and
number of uses; double-click inserts. The rows come from `block_cmds::blockRows`, which is tested
without a window. The ImGui part is compiled only when `TBCAD_BUILD_APP` is on.
