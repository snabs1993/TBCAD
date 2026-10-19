# Dimensions and leaders (W5-5)

Library `libs/dim` (`tbcad::dim`, depends on core, geom and text), command module
`libs/commands/dim` (`tbcad::dimcmd`). Include `tbcad/dim/dim.h`.

A `DimensionData` stores definition points, not geometry. `dim::layoutDimension` turns them and a
`DimStyle` into a `Layout` (lines, arcs, filled arrowheads, one text). The display generator, the
bounds and the explode hook all start from that layout, so they never disagree.

## Definition points

| Kind | `defPts` | Other fields |
| --- | --- | --- |
| Linear | `[0]` first extension line origin, `[1]` second origin, `[2]` a point on the dimension line | `rotation` = angle of the dimension line (0 horizontal, π/2 vertical) |
| Aligned | same | the dimension line is parallel to `[0]`→`[1]` |
| Radius | `[0]` centre, `[1]` point on the circle | |
| Diameter | `[0]` and `[1]` opposite points on the circle | |
| Angular2Line | `[0]`,`[1]` first line, `[2]`,`[3]` second line | `textPos` = a point on the dimension arc |
| Angular3Point | `[0]`,`[1]` the two ends, `[2]` vertex, `[3]` a point on the dimension arc | |

`measurement` is a cache; the generator recomputes from the points (`dim::measurement`: mm, or
radians for angles). `dim::refresh(doc, d)` updates the cache and, unless `textPosUser`, `textPos`
(Angular2Line keeps the arc position there and is never moved). Grips move definition points and the text position.

Dimensions are **associative**: a definition point that was placed on an end, vertex or centre of a
line, polyline, arc, circle or point (an object-snapped point) is linked to it when the dimension is
made, and follows when that object is moved, stretched, rotated, scaled, trimmed or edited with grips.
The measurement and text position are refreshed; a text placed by hand moves along. When the whole measured object is only moved (not stretched or rotated), the free dimension-line point moves with it, so the dimension keeps its offset. The link lives
in `DimensionData::assoc` and is saved in the file. Moving a dimension alone, or copying it, cuts the
link (the point no longer sits where the object's point was). **DIMDISASSOC** cuts the links of the
selected dimensions; **DIMREASSOC** links their points again where they sit exactly on an object's
point. Radius and diameter dimensions are not linked in this version.

For two-line angular dimensions the measured sector is the one of the four formed by the lines
that contains the arc point, so the same two lines give the acute or obtuse angle depending on
where the cursor is.

## Text

`dim::dimensionText`: the measurement in the style's units (`mm`, `cm`, `m`, `in`, `ft`) and
`precision`, rounded to `roundOff` when set, zero suppression (`.5`, `12`), then the kind prefix
(`R` radius, `Ø` diameter), `prefix`, number, `suffix`. Angles show degrees with `°`. A text
override replaces everything, with `<>` standing for the measured text. `overallScale` multiplies
text height, gap, arrow size and the extension distances, never the measurement.

### Alternate units and tolerances

DIMSTYLE has `AltUnits` (On/Off), `AltUnit` (mm, cm, m, in, ft), `AltScale` (a multiplier applied to the
millimetres first), `AltPrecision`, `AltPrefix` and `AltSuffix`: the text gets ` [alternate]` after the
measurement (length dimensions only). `Tolerance` is None, Symmetrical (`100.00 ±0.05`), Deviation
(`100.00 +0.05 -0.02`) or Limits (`100.05 / 99.98`, replacing the measurement), with `TolUpper`, `TolLower`
(millimetres; degrees for angular dimensions) and `TolPrecision`. The order is kind prefix, style prefix,
measurement, style suffix, tolerance, alternate; `<>` in a text override stands for all of it. On the
drawing, Deviation and Limits are stacked like AutoCAD draws them (upper over lower, 0.7 of the text
height, as an MTEXT `\S+0.05^-0.02;` stack when exploded or exported); the fit rules use the stacked
width. `dimensionText` (the style editor, tests) gives the one-line form. Files: `.tbcad` keeps both in an extra DSTX chunk
older programs skip; DXF writes DIMTOL/DIMLIM/DIMTP/DIMTM/DIMTDEC and DIMALT/DIMALTF/DIMALTD/DIMAPOST and reads
them (also in a dimension's own DSTYLE overrides). DXF cannot say "inches" for the alternate value, so it comes
back as millimetres times a factor with the same text.

### Other style variables

`LinearFactor` (DIMLFAC, DXF 144) multiplies the measurement of every length dimension before it is
shown (alternate units, tolerance limits and the `<>` of a text override follow); angles are never
scaled. `Fit` (DIMATFIT, DXF 289) is Best, ArrowsFirst, TextFirst or BothOutside: when text and
arrows do not fit together, Best and ArrowsFirst move the arrows outside first (the default rules
below), TextFirst moves the text outside first, BothOutside moves both. `TextMove` (DIMTMOVE,
DXF 279) is WithLine, Leader or Free: text dragged outside the extension lines (or the circle of a
radius / diameter dimension) gets a leader line, except with Free. All three are edited with
DIMSTYLE, undo with the style, and are kept in `.tbcad` (DSTX), the change journal and DXF.

## Fit rules (linear and aligned)

`L` is the distance between the extension lines, `tw` the text width, `g` the text gap and `a`
the arrow size (all after `overallScale`).

1. `L ≥ tw + 2g + 2a`: arrows and text inside.
2. else `L ≥ tw + 2g`: text inside, arrows outside (pointing in, with a stub of `2a`).
3. else `L ≥ 2a`: arrows inside, text outside beyond the second extension line.
4. else arrows and text outside.

Text placement: above the line (`g` + half the text height), or centred (the dimension line breaks
around the text). Horizontal text on a steep line is centred too. Text outside sits beyond the
line end and the line runs under it (above placement). Text the user moved (`textPosUser`) stays
where it is; outside the extension lines it gets a short leader from the nearest line end. Text
that would read upside down is turned by 180°.

Radius: the line runs from the centre to the circle point with one arrow inside the circle; the
text is placed beyond the circle point (or where the user clicked) with a short leader.
Diameter: both arrows inside; text centred when it fits, else outside. Angular: the arc is drawn
at the arc point's distance from the vertex, extension lines run from the end of each picked line
to just past the arc, the text is on the arc (above, or centred with the arc broken), and arrows
go outside when the arc is shorter than `2a`.

## Leader

Polyline with the arrowhead at the first vertex, a landing of one arrow length towards the text,
and the annotation as MText (markup) attached at `textPos` (left- or right-attached to match the
landing).

## Display and explode

`dim::registerDisplay()` registers Dimension and Leader with `geom::DisplayRegistry` (the command
module does it). Colours: the style's line, extension and text colours; `ByBlock` means the
entity's own colour. `dim::explodeDimension` returns Line / Polyline / Arc entities, solid Hatch
entities for the arrowheads and a Text (MText for leaders); the command module registers it in
`blocks::ExplodeRegistry` for both kinds.

## Commands

| Command | Alias | Flow |
| --- | --- | --- |
| DIMLINEAR | DLI | two points (Enter = pick an object), then the line position. Keywords: Text, Horizontal, Vertical, Rotated. The cursor side picks horizontal or vertical |
| DIMALIGNED | DAL | two points or an object, then the line position. Keyword: Text |
| DIMRADIUS | DRA | pick an arc or circle, then where the text goes. Keyword: Text |
| DIMDIAMETER | DDI | same |
| DIMANGULAR | DAN | two lines then the arc position; Enter at the first prompt = vertex and two points. Keyword: Text |
| LEADER | LE | points, Enter, annotation text |
| DIMSTYLE | D | `Current`, `New`, `Delete`, `Edit`, `List`, `Done` |

DIMSTYLE Edit takes property/value pairs, so a style can be changed from one command line:
`DIMSTYLE Edit Arch TextHeight 3.5 ArrowType Tick Done Done`. Properties: TextHeight, TextGap,
TextStyle, ArrowType (Filled, Open, Tick, Dot, None), ArrowSize, ExtOffset, ExtExtension,
LineExtension, Placement (Above, Centered), Alignment (WithLine, Horizontal), Units, Precision,
Prefix, Suffix (`_` = space, `none` = empty), RoundOff, Scale, LeadingZeros, TrailingZeros
(Suppress, Show), LineColor, ExtColor, TextColor (ByBlock, ByLayer, 0-255 or R,G,B). New styles
copy the current one and become current. Every command is one undo step.

New dimensions take the current dimension style (`Header::currentDimStyle`) and the current
layer. The Dimension Styles panel (application builds) lists the styles with a live preview and
edits through the same command line; it is listed under the panels menu.

## DIM and MLEADER (Q43)

**DIM** dimensions whatever you pick, and keeps asking for the next object until Enter:

| Pick | Result | Keywords while placing |
| --- | --- | --- |
| Line or polyline segment, axis aligned | Linear (horizontal or vertical by cursor side) | Text, Aligned, Angular (lines) |
| Slanted line or segment | Aligned | Text, Linear, Angular (lines) |
| Circle | Diameter | Text, Radius |
| Arc | Radius | Text, Diameter |
| Line, then **Angular**, then a second line | Angular between the lines | Text |

The preview follows the cursor while the location is chosen. All dimensions of one DIM run are one undo step.

**MLEADER** (alias MLD) takes an arrowhead point and a landing point, then asks for the note straight away. Text
typed at either point prompt is the note itself. The first prompt has the option **Content** (**Text**,
**Block**, an existing block inserted at the landing point, or **None**; the multileader style's type is the
default) and the landing prompt has **Add** for further arrowheads. It uses the current dimension style for arrow and text. The result is one
multileader object (`MLeader`): all leader lines, the landing point where they meet, a dogleg of one arrow
length to the note, and the note or block at its end. It moves, copies, scales, grips and erases as one
object, and its lines, landing point and note each have a grip. **MLEADERALIGN** (MLA) lines up the landing
ends of several multileaders (or plain leaders) in a column or a row with a reference one. **MLEADERCOLLECT** (MLC) merges several multileaders with a text note into the first one: every arrow stays, all lines end at its landing and the notes are joined one per line (block notes are left alone). Everything from
one MLEADER run is one undo step. To DXF a multileader is written as one LEADER per line, a line for the
dogleg and the note as MTEXT (or a block as INSERT): other programs show it, but it is no longer one object
there.

**DIMJOGLINE** (DJL) puts a Z-shaped jog in the dimension line of a linear or aligned dimension, the sign
that the drawn length is shortened. Pick the dimension, then the place on the dimension line, or press
Enter for the middle; the keyword **Remove** takes the jogs out. The jog is stored with the dimension, so it
moves, scales and saves with it.

## Arc length and ordinate dimensions (Q81)

- **DIMARC** (DAR) dimensions the length of a picked arc. The dimension arc follows the cursor; **Text** overrides the
  text. In DXF it is written as plain lines, arcs and text (a warning says so); `.tbcad` keeps it as a dimension.
- **DIMJOGGED** (DJO) is a radius dimension for arcs whose centre is far away: pick the arc, a centre override point, the
  dimension line location and the jog position (Enter = midpoint). DXF export writes it as plain lines, arcs and text.
- **DIMCENTER** is another name for CENTERMARK.
- **DIMORDINATE** (DOR) dimensions the X or Y distance of a point from the origin (0,0). Pick the feature, then the
  leader end: moving mostly up or down measures X, mostly sideways measures Y; **Xdatum** / **Ydatum** force the axis.
  Ordinate dimensions round trip through DXF (type 6, bit 64 = X datum).
- **QDIM** dimensions the key points (line ends, polyline vertices, centres) of the selected objects as one chain of linear
  dimensions. Click above or below the objects for horizontal dimensions, beside them for vertical ones. At the position prompt the options are **Continuous** (the default, a chain), **Baseline** (every dimension starts at the first key point and each line sits one step further out) and **Radius** / **Diameter** (one dimension for every circle and arc of the selection).

## Chains and text editing (Q67)

- **DIMCONTINUE** (DCO) continues from the last linear or aligned dimension: the new one starts at the previous
  second extension line and shares its dimension line. Keep picking points; Enter ends. **Select** chooses another
  base dimension.
- **DIMBASELINE** (DBA) starts every new dimension at the base dimension's first extension line and stacks each one
  further out, by 2.5 text heights (scaled by the style's overall scale).
- **DIMTEDIT** (DTE) moves the text of a picked dimension to a new point; **Home** puts it back at its automatic place.
- **DIMEDIT** (DED) works on several dimensions at once: **New** gives them the text you type (`<>` = the measurement), **Home** restores the measured text, the automatic text place and the automatic text angle. **Rotate** fixes the text angle (0 = automatic again). **Oblique** leans the extension lines of linear and aligned dimensions to an angle measured from the X axis (Enter = square again; within 85 degrees of the perpendicular). Both are stored with the dimension (`DimensionData::oblique`, `textRotated`/`textRotation`; file format minor 10, tag 2 of the dimension tail).
- **DIMBREAK** (DBR) opens gaps in the dimension and extension lines where other objects cross them: pick the dimensions, then **Auto** (pick the objects to break with, or Enter / **All** for every object of the drawing) or **Remove**. The gap is 1.5 arrow sizes wide. Gaps are stored with the dimension (a new optional part of the dimension record, file format minor 9; older builds open the file and show the dimension without gaps). They sit at fixed points of the sheet: after the dimension is moved or edited run DIMBREAK again. DXF export and EXPLODE keep the gaps.
- **DIMSPACE** (DSP) evens out the gaps between parallel linear or aligned dimensions: pick the base dimension, select the
  others, then type the spacing or **Auto** (two text heights). A spacing of 0 lines them up with the base.

**MLEADEREDIT** (MLE) adds leader lines to a multileader (**Add**: pick arrowhead points, Enter ends) or removes the
one whose arrowhead is nearest to a pick (**Remove**); one leader line always stays.

## What the commands accept

- DIMLINEAR and DIMALIGNED (and DIM): a line, an arc (its ends), the polyline segment nearest the pick, an
  elliptical arc or spline (end to end; a full ellipse across its major axis).
- DIMRADIUS, DIMDIAMETER, DIMJOGGED: a circle, an arc, or the bulged polyline segment nearest the pick.
- DIMANGULAR: two lines or polyline segments, an arc (the angle it spans), or three points.
- DIMORDINATE measures from the origin of the current UCS (angle 0; a rotated UCS still measures along world axes).
- Changing a dimension or text style redraws every dimension, leader, text and table that uses it, also inside blocks.

## DIMREGEN and DIMINSPECT

`DIMREGEN` brings every associative dimension up to the current position of the points it follows and refreshes its measurement and text. `DIMINSPECT` marks selected dimensions as inspection dimensions: the text becomes `<measurement> [label rate%]` (Add, with an optional label and a rate of 0 to 100), and Remove takes the mark off.

`DIMUPDATE` gives the selected dimensions the current dimension style. `QLEADER` is another name for `LEADER`.
