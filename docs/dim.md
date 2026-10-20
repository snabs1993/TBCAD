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
text height, gap, arrow size and the extension distances, never the measurement. A text style
with a fixed height (the dimension style's own text style) sets the text height instead, as in
AutoCAD: neither the style's text height nor `overallScale` applies to it.

### Alternate units and tolerances

DIMSTYLE has `AltUnits` (On/Off), `AltUnit` (mm, cm, m, in, ft), `AltScale` (a multiplier applied to the
millimetres first), `AltPrecision`, `AltPrefix` and `AltSuffix`: the text gets ` [alternate]` after the
measurement (length dimensions only). `Tolerance` is None, Symmetrical (`100.00 ±0.05`), Deviation
(`100.00 +0.05 -0.02`) or Limits (`100.05 / 99.98`, replacing the measurement), with `TolUpper`, `TolLower`
(millimetres; degrees for angular dimensions) and `TolPrecision`. The order is kind prefix, style prefix,
measurement, style suffix, tolerance, alternate; `<>` in a text override stands for all of it. On the
drawing, Deviation and Limits are stacked like AutoCAD draws them (upper over lower, each line
`TolHeight` (DIMTFAC, default 1) times the text height, as an MTEXT `\S+0.05^-0.02;` stack when
exploded or exported); the fit rules use the stacked width. `dimensionText` (the style editor, tests) gives the one-line form. Files: `.tbcad` keeps both in an extra DSTX chunk
older programs skip; DXF writes DIMTOL/DIMLIM/DIMTP/DIMTM/DIMTDEC and DIMALT/DIMALTF/DIMALTD/DIMAPOST and reads
them (also in a dimension's own DSTYLE overrides). DXF cannot say "inches" for the alternate value, so it comes
back as millimetres times a factor with the same text.

`TextVertPos` (DIMTVP, default 0) moves text centred on the dimension line (DIMTAD 0) that many text
heights above it (negative: below), as in AutoCAD; the line breaks round the text only when the move is
less than 0.7. It is read from DXF and DWG (also from a dimension's own overrides), written to DXF, and
kept in `.tbcad` in the DSTV chunk.

### Other style variables

`LinearFactor` (DIMLFAC, DXF 144) multiplies the measurement of every length dimension before it is
shown (alternate units, tolerance limits and the `<>` of a text override follow); angles are never
scaled. `Fit` (DIMATFIT, DXF 289) is Best, ArrowsFirst, TextFirst or BothOutside: when text and
arrows do not fit together, Best and ArrowsFirst move the arrows outside first (the default rules
below), TextFirst moves the text outside first, BothOutside moves both. `TextMove` (DIMTMOVE,
DXF 279) is WithLine, Leader or Free: text dragged outside the extension lines (or the circle of a
radius / diameter dimension) gets a leader line, except with Free. All three are edited with
DIMSTYLE, undo with the style, and are kept in `.tbcad` (DSTX), the change journal and DXF.

Tick marks (`ArrowType::ArchTick`, DXF `_ARCHTICK` or `_OBLIQUE`, or a DIMTSZ above 0, which
sets the arrow size to DIMTSZ) are drawn as AutoCAD's block: a stroke at 45 degrees from
(-a/2,-a/2) to (a/2,a/2) round the tip, so `a·√2` long.

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
| DIMLINEAR | DLI | two points (Enter = pick an object), then the line position. Keywords: Mtext, Text, Angle, Horizontal, Vertical, Rotated. The cursor side picks horizontal or vertical |
| DIMALIGNED | DAL | two points or an object, then the line position. Keywords: Mtext, Text, Angle |
| DIMRADIUS | DRA | pick an arc or circle, then where the text goes. Keywords: Mtext, Text, Angle |
| DIMDIAMETER | DDI | same |
| DIMANGULAR | DAN | two lines then the arc position; Enter at the first prompt = vertex and two points. Keywords: Mtext, Text, Angle |
| LEADER | LE | points, Enter, annotation text |
| DIMSTYLE | D | `Current`, `New`, `Delete`, `Edit`, `List`, `Done` |

As in AutoCAD, **Mtext** and **Text** set the dimension text (`<>` = the measurement) and **Angle** fixes the text at an angle, the same as DIMEDIT Rotate; DIMARC and DIMORDINATE take them too.

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
typed at either point prompt is the note itself. As in AutoCAD the first prompt also offers **leader Landing first**
(the landing point, then one or more arrowheads, Enter ends), **Content first** (the note or block name, where it goes,
then the arrowheads) and **Options**: **Type** of content (**Text**, **Block**, an existing block inserted at the
landing point, or **None**; the multileader style's type is the default), **Maxpoints** (how many points a leader line
has, arrowhead and landing included: with more than two, each further pick is a bend and Enter makes the last bend the
landing) and **Dogleg** (Yes / No). The landing prompt has **Add** for further arrowheads. It uses the current dimension style for arrow and text. The result is one
multileader object (`MLeader`): all leader lines, the landing point where they meet, a dogleg of one arrow
length to the note, and the note or block at its end. It moves, copies, scales, grips and erases as one
object, and its lines, landing point and note each have a grip. **MLEADERALIGN** (MLA) lines up the landing
ends of several multileaders (or plain leaders) in a column or a row with a reference one; **Spacing** sets the distance between them, **Distribute** spreads the landings evenly between two picked points and **Parallel** turns the last segment of every leader line parallel to the reference's, keeping its length. **MLEADERCOLLECT** (MLC) merges several multileaders with a text note into the first one: every arrow stays, all lines end at its landing and the notes are joined one per line (block notes are left alone). Everything from
one MLEADER run is one undo step. To DXF a multileader is written as one LEADER per line, a line for the
dogleg and the note as MTEXT (or a block as INSERT): other programs show it, but it is no longer one object
there.

**DIMJOGLINE** (DJL) puts a Z-shaped jog in the dimension line of a linear or aligned dimension, the sign
that the drawn length is shortened. Pick the dimension, then the place on the dimension line, or press
Enter for the middle. A dimension has one jog, as in AutoCAD: a new one replaces the old. **Remove** at the
first prompt, then a pick, takes a dimension's jog out (**Remove** at the second prompt does the same for the
dimension just picked). The jog is stored with the dimension, so it moves, scales and saves with it, and it
stays on the dimension line when the line is moved (a grip, DIMSPACE).

## Arc length and ordinate dimensions (Q81)

- **DIMARC** (DAR) dimensions the length of a picked arc. The dimension arc follows the cursor; **Text** overrides the
  text. In DXF it is written as plain lines, arcs and text (a warning says so); `.tbcad` keeps it as a dimension.
- **DIMJOGGED** (DJO) is a radius dimension for arcs whose centre is far away: pick the arc, a centre override point, the
  dimension line location and the jog position (Enter = midpoint). DXF export writes it as plain lines, arcs and text.
- **DIMCENTER** is another name for CENTERMARK.
- **DIMORDINATE** (DOR) dimensions the X or Y distance of a point from the origin (0,0). Pick the feature, then the
  leader end: moving mostly up or down measures X, mostly sideways measures Y; **Xdatum** / **Ydatum** force the axis.
  Ordinate dimensions round trip through DXF (type 6, bit 64 = X datum).
- **QDIM** dimensions the key points (line ends, polyline vertices, centres) of the selected objects in one go. Click above or below the objects for horizontal dimensions, beside them for vertical ones. At the position prompt the options are **Continuous** (the default, a chain), **Staggered** (pairs nested from the outside in: the innermost pair sits at the picked line, each outer pair one step further out), **Baseline** (every dimension starts at the first key point and each line sits one step further out), **Ordinate** (one ordinate dimension per key point, the leaders ending on the picked line), **Radius** / **Diameter** (one dimension for every circle and arc of the selection) **datumPoint** (the point Baseline starts at and Ordinate measures from; the origin by default), **Edit** (pick key points to remove them, or **Add** points, until **eXit** or Enter) and **seTtings** (**Intersection** also dimensions the points where the selected objects cross; **Endpoint**, the default, only their own key points). As in AutoCAD, QDIM also edits existing dimensions: linear, aligned and ordinate dimensions in the selection give their extension line origins as key points and are replaced by the new set.

## Chains and text editing (Q67)

- **DIMCONTINUE** (DCO) continues from the last linear, aligned, angular or ordinate dimension. A linear or aligned
  one starts at the previous second extension line and shares its dimension line; an angular one goes on around the
  same vertex at the same radius, from the side of the previous arc nearest the pick; an ordinate one keeps the datum
  and axis and lines its leader end up with the previous one. Keep picking points; Enter ends. **Undo** takes back the
  last dimension of the run and **Select** chooses another base dimension.
- **DIMBASELINE** (DBA) starts every new dimension at the base dimension's first extension line (for an angular one,
  the side away from the pick) and stacks each one further out by the style's baseline spacing (DIMDLI), or
  2.5 text heights when that is 0, scaled by the overall scale. It has the same **Undo** and **Select** options.
- **DIMTEDIT** (DTE) moves the text of a picked dimension to a new point; **Home** puts it back at its automatic place.
- **DIMEDIT** (DED) works on several dimensions at once: **New** gives them the text you type (`<>` = the measurement), **Home** restores the measured text, the automatic text place and the automatic text angle. **Rotate** fixes the text angle (0 = automatic again). **Oblique** leans the extension lines of linear and aligned dimensions to an angle measured from the X axis (Enter = square again; within 85 degrees of the perpendicular). Both are stored with the dimension (`DimensionData::oblique`, `textRotated`/`textRotation`; file format minor 10, tag 2 of the dimension tail).
- **DIMBREAK** (DBR) opens gaps in the dimension and extension lines where other objects cross them: pick the dimensions, then **Auto** (pick the objects to break with, or Enter / **All** for every object of the drawing) **Manual** (one dimension: pick two points on a dimension or extension line; the gap between them is added to the breaks it already has) or **Remove**. An automatic gap is 1.5 arrow sizes wide. As in AutoCAD, the arcs of angular and arc length dimensions break too, and the objects that break a dimension include other dimensions, leaders, blocks and text as well as lines and curves (hatches and points do not). Text takes out the whole part of the line under its box; a line that only ends on the dimension, such as the shared extension line of a chained dimension, makes no gap. Gaps are stored with the dimension (a new optional part of the dimension record, file format minor 9; older builds open the file and show the dimension without gaps). As in AutoCAD, automatic gaps follow: the dimension remembers the objects that broke it, and when it or one of them is moved or edited the gaps are worked out again (an object erased no longer breaks it). Gaps placed by hand (**Manual**) stay at fixed points of the sheet, and Auto keeps them. A copy of a broken dimension keeps its gaps where they are. DXF export and EXPLODE keep the gaps. Leaders and multileaders break the same way (their leader lines, hook and dogleg; file format minor 18).
- **DIMSPACE** (DSP) evens out the gaps between parallel linear or aligned dimensions: pick the base dimension, select the
  others, then type the spacing or **Auto** (two text heights). A spacing of 0 lines them up with the base.
  With an angular base dimension it spaces the angular dimensions about the same vertex by their arc radius.

**MLEADEREDIT** (MLE) adds leader lines to a multileader (**Add**: pick arrowhead points, Enter ends) or removes the
one whose arrowhead is nearest to a pick (**Remove**); one leader line always stays. **AIMLEADEREDITADD** and **AIMLEADEREDITREMOVE** are the same with the choice already made.

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
