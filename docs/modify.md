# modify: offset, trim, extend, fillet, chamfer, break, join

Two modules (W5-2):

* `libs/modify` (`tbcad::modify`, depends on `tbcad::core` and `tbcad::geom`) holds the algorithms as
  pure functions on `Entity` values. Include `tbcad/modify/modify.h`.
* `libs/commands/modify` (target `tbcad_modify_commands`, namespace `tbcad::modifycmd`) registers the
  commands. The target is not `tbcad_modify` because that is the algorithm library.

| Command | Alias | Function |
| --- | --- | --- |
| `OFFSET` | `O` | `offsetEntity`, `offsetToward`, `offsetThrough`; reports Erase/Layer/Corners, `Exit` and `Undo` (last offset) at both prompts, Enter on the side ends unless Multiple |
| `TRIM` | `TR` | `trimEntity` |
| `EXTEND` | `EX` | `extendEntity` |
| `FILLET` | `F` | `filletEntities` (also rays and construction lines: the kept side becomes a ray, or a line towards a ray's start), `filletPolylineVertex`, `filletPolyline`; reports the current mode and radius, `Undo` takes back the last corner |
| `CHAMFER` | `CHA` | `chamferEntities` (rays and construction lines as for FILLET), `chamferPolylineVertex`, `chamferPolyline`; reports the current mode and distances, `Undo` takes back the last corner, `mEthod` (E) since M is Multiple |
| `BREAK` | `BR` | `breakEntity` (a construction line becomes two rays, a ray a line plus a new ray) |
| `BREAKATPOINT` | `BAP` | `breakAtPoint` |
| `JOIN` | `J` | `joinEntities`. A single arc or elliptical arc asks `[cLose/eXit]`: cLose makes it a full circle or ellipse. |
| `ERASE` | `E` | (only registered when nobody else did) |
| `OOPS` | | puts back the objects the last ERASE removed, even after other commands (kept per drawing, for the session only). |

Every algorithm returns the new geometry and touches nothing; the command applies it to the
document in one transaction (one undo step). Results keep every property of the source entity
(layer, colour, linetype, lineweight, ...). `EntityEdit` says what to do with the source: replace its
data (the id is kept, so selections and later associations survive), add more entities, or erase it.

## Offset

`signedDistance > 0` is to the **left** of the direction of travel (inside a counter-clockwise
circle, arc, ellipse or closed polyline), `< 0` to the right. `offsetToward` picks the sign from a
point, `offsetThrough` also takes the distance from it.

* Line, circle, arc: exact. A circle or arc that would collapse is an error.
* Polyline: every segment is offset on its own, corners are joined, then the raw chain is cut at all
  its self-intersections and only the pieces that stay at least `|d|` away from the source are kept
  (loops and collapsed parts disappear; a pinched-off result becomes several polylines; nothing left
  is an error). Corners where the offset curves part get a circular arc (`roundCorners`, centred on
  the source vertex) or a sharp mitre; corners where they overlap are always sharp.
* Ellipse and spline: offset sample points (within `tolerance`, default 0.01 mm) interpolated by a
  cubic spline. Distances larger than the radius of curvature produce cusps; nothing is done about
  them.

The command remembers the distance (`offset.distance`) and the corner type (`offset.roundcorners`,
default sharp, option `Corners`).

## Trim and extend

Edges are passed in; the command uses every visible curve on a layer that is on and not frozen
(fast mode), or the selection made at the first prompt. `TrimOptions::extendEdges` (command option
`Edge`) lets the edges run on (lines to infinity, arcs and circles to full circles).

* Trim removes the stretch under the pick between the nearest cuts on both sides. One cut on one side
  removes everything from the pick to that end. Circles become arcs (two cuts needed), closed
  polylines are opened at the cut. Lines, arcs, circles, ellipses and polylines.
* Extend moves the end nearest to the pick to the nearest boundary ahead of it. Lines, arcs and open
  polylines (first or last segment).
* Shift while picking swaps the two (the pick result carries `shift`, see below). `Fence` and
  `Crossing` pick many objects at once; `Undo` takes back the last step.

## Fillet and chamfer

* Fillet between lines, arcs and circles: the four offset combinations of the two curves are
  intersected, and the centre whose tangent points lie nearest the two pick points wins. The inputs are
  trimmed or extended to the tangent points, keeping the part that holds the pick (circles are never
  trimmed). Radius 0 extends or trims both to their intersection (Shift on the second pick does that
  for one corner). Parallel lines get a semicircle at the end nearest the first pick.
* Polyline corners are rounded by picking two neighbouring segments of the same polyline, or
  everything with `Polyline`. A corner whose neighbours are too short for the radius is an error for a
  single corner and skipped in the whole-polyline form.
* Chamfer on straight lines and polyline corners: distances `d1`, `d2`, or `d1` plus the angle between
  the chamfer and the first line.
* Settings: `fillet.radius`, `fillet.trim`, `chamfer.distance1`, `chamfer.distance2`, `chamfer.angle`,
  `chamfer.byangle`, `chamfer.trim`.

## Break and join

* `breakEntity(e, p1, p2)` removes the part between the points (either order). A circle loses the
  arc running counter-clockwise from the first to the second point (AutoCAD's rule), so the remaining
  arc runs from the second point counter-clockwise to the first. A closed polyline opens.
* `breakAtPoint` splits in two; circles need two points.
* `joinEntities` also joins elliptical arcs of one ellipse (a full turn becomes an ellipse) and clamped splines whose ends touch (one spline with a C0 joint, same curve; fit-only splines are interpolated first, and lines that touch an end continue the spline as a straight run), and `EXPLODE` breaks multiline text into one single-line text per line.
* `joinEntities` first fuses collinear lines and arcs of one circle (overlapping or touching), then
  chains line, arc and open-polyline pieces whose ends are within `tolerance` (command setting
  `join.tolerance`, default 0.001 mm) into a polyline, closed when the chain closes. Two arcs that
  complete a circle give a circle. The result takes the properties of the first entity in the list.

## Notes for the application

* `cmd::EntityResult::shift` and `cmd::Input::pick(id, at, shift)` are new in `libs/cmd`. The UI has to
  pass the Shift state with entity picks (`libs/ui/src/app_core.cpp`, where it calls `Input::pick`) for
  the TRIM/EXTEND swap to work in the app.
* Polylines are handled on a cleaned copy (`cleanPolyline` drops coincident vertices), so vertex
  indices of `filletPolylineVertex` refer to that copy. Per-vertex widths are not kept by the editing
  operations (the constant width is).

## More modify commands (Q70)

| Command | Alias | What it does |
| --- | --- | --- |
| `LENGTHEN` | `LEN` | Until a mode is chosen a pick only measures (`Current length`, and the included angle of an arc). DElta, Percent, Total or DYnamic (with a preview) then change a line, arc, elliptical arc, open polyline or spline at the end nearest the pick; DElta and Total take an `Angle` for arcs. `Undo` takes back the last change. Shortening may drop whole trailing polyline segments. |
| `REVERSE` | | Reverses lines, polylines and splines (arcs and circles have no direction here). |
| `DIVIDE` | `DIV` | Points that split an object into N equal parts (N-1 points on an open curve, N on a closed one). |
| `MEASURE` | `ME` | Points every L mm, starting at the end nearest the pick. |
| `ALIGN` | `AL` | One or two source/destination point pairs, optional scaling for the second pair. |
| `MATCHPROP` | `MA` | Copies layer, level, colour, linetype, linetype scale, lineweight, transparency and plot style, plus text height/style (also between text and MText), polyline width, hatch pattern/scale/angle/fill, dimension and leader style, table style and cell styles, multileader style, and the annotative flag and scales, from one object to others. The destination prompt takes picks or windows, so many objects match at once. `Settings` switches each property on and off on its own (Color, LAyer, LType, LTScale, LWeight, TRansparency, PStyle, Text, Dimension, Hatch, POlyline, TAble, Multileader, ANnotative; `Basic` toggles the first seven together); the choice is remembered. |
| `OVERKILL` | | Deletes duplicates (lines, circles, arcs, points, ellipses, polylines, splines, text, mtext and inserts; lines also reversed) and merges overlapping or touching collinear lines and arcs of one circle. With `overkill.optimize` set it also removes repeated vertices and vertices on a straight run from polylines of one width. Settings: `overkill.tolerance`, `overkill.ignoreproperties`, `overkill.overlapping`, `overkill.endtoend`, `overkill.optimize`. Lines are grouped by direction and offset, so tens of thousands of parallel lines are quick. |
| `PEDIT` | `PE` | Turn a line or arc into a polyline, Close/Open, Join (with a Fuzz distance; `Fuzz` > `Jointype` Extend/Add/Both decides whether a gap is closed by extending the two segments to meet, by a new segment, or extending where it can — saved in `pedit.jointype`), Width, Fit (each segment becomes two tangent-continuous arcs through the vertices), Ltypegen (OFF restarts a dashed pattern at every vertex; ON runs it on, the default), Reverse, Spline (polyline to spline entity), Decurve (spline to polyline; on a polyline it straightens every arc segment) and Undo (takes back the last option). `Multiple` at the first prompt applies Close/Open/Join/Fuzz/Width/Spline/Decurve/Reverse to every selected object. |

DIVIDE and MEASURE also take the option **Block**: they insert a named block at each division instead of a point, turned along the object when you answer Yes to "Align block with object?".

The algorithms live in `libs/modify/extra.h`. POINT objects made by DIVIDE and MEASURE take the layer of the source object.

### Batch 2 (Q71m)

| Command | Alias | What it does |
| --- | --- | --- |
| `BLEND` | `BLE` | Tangent spline between the ends (nearest each pick) of two open lines, arcs, polylines or splines. |
| `EXTRIM` | | Pick a cutting edge (line, circle, arc or closed polyline) and a side; every selected object is cut at the edge and the part on that side is removed. |
| `SCALETEXT` | | Scale factor, New height or Match for text and multiline text; the insertion point stays. |
| `SETBYLAYER` | | Colour, linetype and lineweight back to ByLayer. |

ALIGN, MATCHPROP, SETBYLAYER and SCALETEXT accept every entity kind on an editable layer; the others need curve geometry.

### Batch 3 (Q72m)

| Command | Alias | What it does |
| --- | --- | --- |
| `SPLINEDIT` | `SPE` | Add, Delete or Move fit points (the spline is fitted again), Tolerance (fits the spline again through as few fit points as keep it within a distance of the old curve), Close, Open, Reverse. On control-vertex splines Add/Delete/Move/Weight edit the vertices, `ELevate` raises the order (degree + 1, up to 26) without changing the curve, `Purge` drops fit points and `Polyline` converts. `Join` takes in lines, arcs, polylines and splines touching an end; `Undo` takes back the last option. |
| `JUSTIFYTEXT` | | Changes the justification of Text (Left, Align, Fit, Center, Middle, Right, TL..BR) or the attachment of MText without moving it. Align and Fit span the text between where it now starts and ends; Aligned or Fit text turned into another justification keeps its size (its height or width factor takes the stretch). The Justify row of the Properties panel works the same way. |
| `PEDIT` > Vertices | | Insert (splits a straight or arc segment), Remove (neighbours joined by a straight segment) and Move a vertex. Per-vertex widths are not preserved when inserting. |

The menu now lists the new commands under Modify (Draw for DIVIDE and MEASURE).

## FIND, RENAME, COPYTOLAYER (Q75m)

- **FIND** (Ctrl+F): asks for the text, the replacement (Enter = only count) and Match case. Searches text, mtext, leader notes, dimension text overrides, block attribute values and table cells, in model space and on every layout sheet. One undo step. Objects on locked, off or frozen layers are counted but not changed.
- **RENAME** (REN): Layer, Block, TextStyle or DimStyle, old name, new name. Refuses clashes and layer 0.
- **COPYTOLAYER**: select objects, pick an object on the destination layer; copies land there.
