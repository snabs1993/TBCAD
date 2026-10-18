# Multilines, rays and construction lines

## MLINE (ML) and MLSTYLE
MLINE draws several parallel lines along one path, for walls. At the first prompt you can set
`Justification` (Top, Zero, Bottom), `Scale` and `STyle`; the choices are kept for the session.
Then pick points; `Close` joins the last point to the first, `Undo` removes the last point, Enter
finishes. Corners are mitred.

MLSTYLE defines styles: `New`, `Offsets` (distances of the lines from the path, positive to the
left, e.g. `1 0 -1`), `Caps` (None/Start/End/Both), `Delete`, `Current`, `List`. STANDARD (two lines,
1 unit apart) always exists. Styles live in the drawing and are saved with it.
EXPLODE turns a multiline into plain polylines.

DXF: a multiline is written as an anonymous block of polylines (so other programs show it) plus
extended data that TBCAD reads back to recover the multiline and its style exactly.

## RAY and XLINE (XL)
RAY starts at a point and goes on forever; XLINE goes on forever both ways. XLINE options: `Hor`,
`Ver`, `Ang`, `Bisect`, `Offset`. They are drawn very long, are left out of Zoom Extents and plot
extents, and snap only at the ray's start point. DXF: written as native RAY / XLINE (R12 files get
a LINE).

## Regions and sketches
See regions.md.

## MLEDIT

`MLEDIT` fixes joints: **Corner** (pick two multilines near the ends that should meet; both centre lines are extended or cut back to where they cross), **Add** a vertex (it goes into the nearest segment), **Delete** the vertex nearest the pick (a multiline keeps at least two) and **Close** or **Open** the path. **Cut** opens a gap between two points of one stretch (for all lines, or **Single** for the line nearest the first point) and **Weld** closes gaps again. **Cross** (Open, Closed, Merged) and **Tee** (Open, Closed) pick two multilines: Closed cuts the first wall where the second one passes, Open also opens the outer lines of the second, Merged lets the outer lines of both run through and stops the inner ones. A Tee brings the end of the first wall to the near edge of the second. Gaps are stored with the multiline (`MlineData::cuts`: stretch, start and end fraction, first and last line) and are saved in an optional tail of the record; older builds open the file and show the multiline without gaps. Add, Delete and Open remove or move the gaps of the stretches they change. Gaps are fixed to the stretch, so after moving a vertex check them again.
