# geom: geometry algorithms

`libs/geom` (`tbcad::geom`) holds pure functions on entity data. It depends only on `tbcad::core`.
Include `tbcad/geom/geom.h` for everything, or the individual headers `curve.h`, `spline.h` and
`entity.h`.

## Parameter conventions

Every curve has a parameter `t`; `CurveRef::t0()` and `t1()` give its range.

| Curve | `t` | Range |
| --- | --- | --- |
| Line | linear, 0 at `a`, 1 at `b` | `[0, 1]` |
| Arc / circle | angle in radians, counter-clockwise from +X | `[startAngle, startAngle + sweep]` (a circle has sweep 2π) |
| Ellipse | parametric angle: `center + major·cos t + perp(major)·ratio·sin t` | `[start, start + sweep]` |
| Spline | knot parameter | `[knots[degree], knots[n]]` |
| Polyline (entity level) | `segmentIndex + local`, local in `[0, 1]` along the direction of travel | `[0, segmentCount]` |

`CurveRef::subcurve` re-normalises lines to `[0, 1]`; arcs, ellipses and splines keep their
parameter values. `wrapParam(t)` maps a periodic parameter into `[t0, t0 + 2π)`.

Angles in `ArcData` are normalised to `[0, 2π)`. A full circle is a `CircleData`; `ArcData` with
equal angles is read as a full circle by geom, but never written.

## Bulge arcs

`bulge = tan(sweep / 4)`, positive = counter-clockwise (the arc bulges to the right of the
direction of travel). `bulgeArc(p0, p1, bulge)` returns the centre, radius, counter-clockwise
start angle and sweep, plus `reversed` when the polyline travels the arc clockwise.
`bulgeThrough(a, b, c)` is the inverse (the bulge of the arc a → b → c).

## Intersections

`intersect(a, b, out, extendA, extendB)` appends `Intersection {p, tA, tB}`.

* line–line, line–circle/arc, circle/arc–circle/arc, line–ellipse: closed form.
* ellipse–ellipse, ellipse–arc and everything with a spline: both curves are flattened with
  per-piece deviation bounds, overlapping pieces are refined with a damped Newton iteration to
  about 1e-9 mm, and duplicates are merged within `tol::kLinear`.
* Lines are parallel when `|cross| ≤ kAngular·|a|·|b|`; collinear overlaps report the end points of
  the overlap. Tangent contacts report one point.
* `extend*` makes a line unbounded, an arc a full circle and an ellipse a full ellipse. Splines are
  not extended.
* `intersectEntities` works on whole entities (polylines by segment). With `extend` only an open
  polyline's first segment (backwards) and last segment (forwards) extend.

## Display

`buildDisplay(doc, entity, ctx, out)` tessellates with a chord tolerance of
`ctx.pixelSizeMm · 0.25` (or `ctx.chordTolMm`). Arcs use
`ceil(sweep / (2·acos(1 − tol/r)))` segments, clamped to 4…4096; ellipses and splines are
subdivided adaptively. Colour, lineweight and linetype are resolved from the entity, its layer
(`doc.effective`) and `DisplayContext::byBlock*`. Layer visibility is **not** applied: the renderer
filters on the layer id in the style so toggling a layer needs no rebuild.

Later modules register generators for their kinds (text, dimensions, hatch, inserts):

```cpp
geom::DisplayRegistry::set(EntityKind::Text, displayFn, boundsFn /* optional */);
```

Without a `boundsFn`, bounds come from the generated display list. Built-in kinds cannot be
overridden. Register at start-up, before concurrent use.

Wide polylines become one `DisplayFill` per segment (no mitred joins).

## Grips

`gripPoints(entity)` returns points in this fixed order; `moveGrip(entity, index, to)` uses the same
indices.

| Kind | Grips |
| --- | --- |
| Line | 0 start, 1 end, 2 midpoint (moves the whole line) |
| Circle | 0 centre (moves), 1 right, 2 top, 3 left, 4 bottom (set the radius) |
| Arc | 0 start, 1 end, 2 midpoint (each re-fits a three-point arc through the other two), 3 centre (moves) |
| Polyline | 0…n-1 vertices, then one midpoint per segment (dragging it sets the bulge so the arc passes through the point) |
| Ellipse | 0 centre, 1 +major, 2 +minor, 3 −major, 4 −minor, then 5 start and 6 end for elliptical arcs |
| Spline | the fit points when present, otherwise the control points (moving a fit point re-interpolates) |
| Text, MText, Insert | 0 position |
| Dimension | linear and aligned: 0, 1 extension line origins, 2, 3 dimension line ends (move the line), 4 text; radius, diameter and ordinate: 0, 1 `defPts`, 2 text; others: 0…3 `defPts`, 4 text |
| Leader | vertices, then the text position |

## Transforms

`transformed(entity, t)` applies an affine map. A circle or arc under a non-uniform scale becomes
an ellipse (canonicalised so the major axis is the longer one). Refused with an error: non-uniform
scale of text, MText, dimensions and leaders, of polylines that contain arc segments (explode them
first), and of inserts whose instance transform would skew. Under a mirror, text keeps its rotation
(the position moves, glyphs are not flipped).

## Splines

`SplineCurve` evaluates B-splines and rational NURBS (degree 1…15) with de Boor's algorithm.
`interpolateSpline(fitPoints, options)` does global interpolation with chord-length
parameterisation and either natural (zero curvature) or tangent end conditions; `closed = true`
gives a C1 closed curve. `splineSubcurve(data, ta, tb)` extracts a clamped piece by knot insertion.

## REVCLOUD

**REVCLOUD** (REVC) draws a revision cloud of 120 degree arcs. As in AutoCAD, Enter at the first
prompt picks an **Object** (a closed polyline, circle, ellipse or spline, which the cloud replaces;
**Reverse** turns the arcs inwards). A point starts a cloud in the current mode, which
**Rectangular**, **Polygonal** and **Freehand** set and the command remembers (REVCLOUDCREATEMODE):
Rectangular takes two corners, Polygonal takes points until Enter closes the cloud, and Freehand
lays an arc each time the cursor moves an arc length along its path. In Freehand mode, picking near
the start closes the cloud; Enter ends an open one and asks whether to reverse its arcs.
**Modify** reshapes a cloud (any closed polyline): pick it where the new part starts, give points
until one lands on it again (or press Enter), pick the side of the old outline to erase, and
answer Reverse; the new part is cloud arcs facing out of the result, and it is one undo step.
**Arclength** sets the longest arc chord (remembered); **Style** chooses Normal or
Calligraphy (each arc swells and thins like a pen stroke, stored as polyline widths).

A cloud stays editable after it is drawn. REVCLOUD marks it with XDATA of the application
`TBCAD_REVCLOUD` (arc length, the longest chord when it was made, and flags), which .tbcad and DXF
keep. The Properties panel shows **Cloud arc length**, **Arcs inward** (closed clouds) and
**Pen-stroke arcs** (the Calligraphy style); changing one lays the arcs again along the same
outline (also `-PROP Set cloudarclength 10`). The cloud's grips are the corners of its outline
(the vertices that are not on a straight run) and the middle of each outline edge: dragging one,
or STRETCH, moves the outline and lays the arcs again. SCALE takes the arc length along. Clouds
drawn before the mark are found by their arcs (all 120 degrees, all one way).
