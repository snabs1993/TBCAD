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
redo like any header change. Entity ids are never reused (PR #53), so references stay valid. A
constraint with a formula ends with `|formula`. Parameters are in `Header::custom["tbcad.parameters"]`,
one `name<TAB>formula` per line.

In DXF both are written as XRECORDs `TBCAD_CONSTRAINTS` and `TBCAD_PARAMETERS` in the root
dictionary (one code-1 group per line) and read back by our importer; other programs ignore them.
ACAD_ASSOCNETWORK is not written: it is an undocumented binary-heavy format and would not carry the
formulas.

## Not done yet

Constraints on polylines, ellipses and splines; point-on-object, symmetric and concentric
constraints; constraint display icons and a parameter panel in the UI.
