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
