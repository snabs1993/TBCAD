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

## Commands

- `GEOMCON` (`GCON`): pick the type, then the objects. For Coincident and the two-point forms the
  point of the object nearest to the click is used.
- `DIMCON` (`DCON`): Distance, Length, Radius, Diameter or Angle; the prompt default is the current
  value.
- `SHOWCON`: lists the constraints. `DELCON`: removes the constraints of the selected objects.
  `SOLVECON`: solves again.

A new constraint is solved at once; if the constraints cannot all hold, nothing changes and the
message names the constraint that conflicts. The message also says how many degrees of freedom
are left.

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
(`id kind entityA partA entityB partB value`). It is saved in the .tbcad file and follows undo and
redo like any header change. Entity ids are never reused (PR #53), so references stay valid.

## Not done yet

Constraints on polylines, ellipses and splines; point-on-object, symmetric and concentric
constraints; constraint display icons and a dimension-driven parameter table; DXF export (the
constraints are not written to DXF).
