# Parametric constraints (Q54)

Geometric and dimensional constraints between lines, circles, arcs and points, solved by our own
solver (`libs/constraints`, no third-party solver). Commands live in `libs/commands/constraints`.

## Constraints

| Kind | Objects | Meaning |
|---|---|---|
| Coincident | two points (line end, arc end or centre, circle centre, POINT) | the points are the same |
| Horizontal / Vertical | one line, or two points | same Y / same X |
| Parallel / Perpendicular | two lines | |
| Equal | two lines, or two circles/arcs | same length / same radius |
| Tangent | line and circle/arc, or two circles/arcs | circle-circle keeps outside or inside tangency, whichever the drawing is closer to |
| Fix | any object or one point | stays where it is |
| Distance | two points | length in mm (also the length of a line) |
| Radius | circle or arc | radius in mm (DIMCON also takes a diameter) |
| Angle | two lines | angle from the first line's direction to the second's |
| Concentric | circles, arcs or points | the centres coincide |
| PointOnObject | a point and a line, circle or arc | the point lies on the line's infinite extension or the full circle |
| Midpoint | a point and a line | the point sits at the middle of the line |
| Collinear | two lines | both lie on one common line |

## Which objects

Lines, circles, arcs and points; **polylines** (every straight segment, a segment with a bulge is
refused; the vertices are points, and the other vertices and the bulges stay as they are);
**ellipses** (the centre is a point; the axes are kept); **inserts** (the insertion point is a point,
the x axis of the insert is a direction, so Parallel, Perpendicular, Angle, Horizontal and Vertical
work on an insert). Inserts with constraints are how block instances are mated: Fix one, then
Coincident / Distance / Parallel the others to it. A reference carries an `index`: the vertex (point)
or segment (Whole) of a polyline, counted from 0. In the commands the vertex or segment nearest to
the click is used.

## Commands

- `GEOMCON` (`GCON`): pick the type (also Concentric, PointOn, Midpoint, Collinear), then the objects. For Coincident and the two-point forms the
  point of the object nearest to the click is used.
- `DIMCON` (`DCON`): Distance, Length, Radius, Diameter or Angle; the prompt default is the current
  value.
- `GEOMCONSTRAINT` and `DIMCONSTRAINT` are the long names of `GEOMCON` and `DIMCON`.
- `AUTOCONSTRAIN` (`AUTOCON`): select objects; adds every Coincident, Horizontal, Vertical,
  Parallel, Perpendicular, Equal and Tangent constraint the drawing already satisfies (within 0.01
  mm / 0.01 rad). Each candidate is trial-solved first, redundant ones are left out, and one undo
  removes them all.
- `PARAMETERS` (`PARAM`): Set a named value or formula (`Height = Width * 0.5`), Delete, List. A
  change re-evaluates every formula constraint and solves, so the geometry follows; a parameter that
  would break the solve is refused. In `DIMCON` the Formula option takes an expression over the
  parameters (angles in degrees) instead of a number.
- `SHOWCON`: lists the constraints. `DELCON`: removes the constraints of the selected objects.
  `SOLVECON`: solves again.

A new constraint is solved at once; if the constraints cannot all hold, nothing changes and the
message names the constraint that conflicts. The message also says how many degrees of freedom
are left.

## Checking the constraints

`analyze` (library), `SHOWCON`, `CONSTRAINTBAR` and the MCP tool `analyze_constraints` report: whether
the constraints can all hold, the degrees of freedom left (overall and per object; 0 = fully
constrained), constraints that are **redundant** (the others already imply them: over-constrained but
consistent) and the constraint that **conflicts** when there is no solution. `DELCONSTRAINT` removes
constraints by the numbers `SHOWCON` shows; `DELCON` removes those of selected objects.

## Behaviour when objects are edited

A commit hook runs after every command: the objects the command touched stay as the user left
them (they are *pinned*) and the other constrained objects follow. If that has no solution the edit
stands and the other objects stay where they were. Constraints on erased objects are dropped.
Everything is part of the command's transaction, so one undo step undoes the edit and the
adjustments together.

## Solver

Levenberg-Marquardt over the coordinates of the constrained objects (line: 4, circle: 3, arc: 5,
point: 2) with a central-difference Jacobian and a Cholesky step. It starts from the current
geometry and damps steps, so the solution is a small change of the drawing. Degrees of freedom
are the number of free variables minus the rank of the Jacobian. Residuals: lengths in mm,
parallel/perpendicular normalised by the line lengths, angles wrapped to (-pi, pi].

## Storage

The list is kept in `Header::custom["tbcad.constraints"]`, one line per constraint
(`id kind entityA partA entityB partB value indexA indexB`; older files without the indices still load). It is saved in the .tbcad file and follows undo and
redo like any header change. Entity ids are never reused (PR #53), so references stay valid. A
constraint with a formula ends with `|formula`. Parameters are in `Header::custom["tbcad.parameters"]`,
one `name<TAB>formula` per line.

In DXF both are written as XRECORDs `TBCAD_CONSTRAINTS` and `TBCAD_PARAMETERS` in the root
dictionary (one code-1 group per line) and read back by our importer; other programs ignore them.
ACAD_ASSOCNETWORK is not written: it is an undocumented binary-heavy format and would not carry the
formulas.

## Not done yet

Constraints on splines and on polyline arc segments; symmetric constraints; constraint display icons and a parameter panel in the UI.
