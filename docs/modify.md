# modify: offset, trim, extend, fillet, chamfer, break, join

Two modules (W5-2):

* `libs/modify` (`tbcad::modify`, depends on `tbcad::core` and `tbcad::geom`) holds the algorithms as
  pure functions on `Entity` values. Include `tbcad/modify/modify.h`.
* `libs/commands/modify` (target `tbcad_modify_commands`, namespace `tbcad::modifycmd`) registers the
  commands. The target is not `tbcad_modify` because that is the algorithm library.

| Command | Alias | Function |
| --- | --- | --- |
| `OFFSET` | `O` | `offsetEntity`, `offsetToward`, `offsetThrough` |
| `TRIM` | `TR` | `trimEntity` |
| `EXTEND` | `EX` | `extendEntity` |
| `FILLET` | `F` | `filletEntities`, `filletPolylineVertex`, `filletPolyline` |
| `CHAMFER` | `CHA` | `chamferEntities`, `chamferPolylineVertex`, `chamferPolyline` |
| `BREAK` | `BR` | `breakEntity` |
| `BREAKATPOINT` | `BAP` | `breakAtPoint` |
| `JOIN` | `J` | `joinEntities` |
| `ERASE` | `E` | (only registered when nobody else did) |

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
| `LENGTHEN` | `LEN` | Delta, Percent, Total or Dynamic change of a line, arc or open polyline at the end nearest the pick. Shortening may drop whole trailing polyline segments. |
| `REVERSE` | | Reverses lines, polylines and splines (arcs and circles have no direction here). |
| `DIVIDE` | `DIV` | Points that split an object into N equal parts (N-1 points on an open curve, N on a closed one). |
| `MEASURE` | `ME` | Points every L mm, starting at the end nearest the pick. |
| `ALIGN` | `AL` | One or two source/destination point pairs, optional scaling for the second pair. |
| `MATCHPROP` | `MA` | Copies layer, colour, linetype, linetype scale and lineweight, plus text height/style and polyline width, from one object to others. |
| `OVERKILL` | | Deletes duplicates (lines, circles, arcs, points, ellipses, polylines, also reversed) and merges overlapping or touching collinear lines and arcs of one circle. Settings: `overkill.tolerance`, `overkill.ignoreproperties`, `overkill.overlapping`, `overkill.endtoend`. |
| `PEDIT` | `PE` | Turn a line or arc into a polyline, Close/Open, Join, Width, Reverse, Spline (polyline to spline entity) and Decurve (spline to polyline). Per-vertex editing is not included yet. |

The algorithms live in `libs/modify/extra.h`. POINT objects made by DIVIDE and MEASURE take the layer of the source object.

### Batch 2 (Q71m)

| Command | Alias | What it does |
| --- | --- | --- |
| `BLEND` | `BLE` | Tangent spline between the ends (nearest each pick) of two open lines, arcs, polylines or splines. |
| `EXTRIM` | | Pick a cutting edge (line, circle, arc or closed polyline) and a side; every selected object is cut at the edge and the part on that side is removed. |
| `SCALETEXT` | | Scale factor, New height or Match for text and multiline text; the insertion point stays. |
| `SETBYLAYER` | | Colour, linetype and lineweight back to ByLayer. |

ALIGN, MATCHPROP, SETBYLAYER and SCALETEXT accept every entity kind on an editable layer; the others need curve geometry.
