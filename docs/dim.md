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
(Angular2Line keeps the arc position there and is never moved). Dimensions are not associative to
geometry in v1: grips move definition points and the text position.

For two-line angular dimensions the measured sector is the one of the four formed by the lines
that contains the arc point, so the same two lines give the acute or obtuse angle depending on
where the cursor is.

## Text

`dim::dimensionText`: the measurement in the style's units (`mm`, `cm`, `m`, `in`, `ft`) and
`precision`, rounded to `roundOff` when set, zero suppression (`.5`, `12`), then the kind prefix
(`R` radius, `Ø` diameter), `prefix`, number, `suffix`. Angles show degrees with `°`. A text
override replaces everything, with `<>` standing for the measured text. `overallScale` multiplies
text height, gap, arrow size and the extension distances, never the measurement.

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

**MLEADER** (alias MLD) takes an arrowhead point and a landing point, then any number of further arrowhead
points (Enter to finish), then the content: **Text**, **Block** (an existing block, inserted at the landing
point) or **None**. It uses the current dimension style for arrow and text. Each arrowhead is a `Leader`
entity; the first one carries the note and the others end at the same landing point, so they move, erase
and export to DXF like any other leaders. Everything from one MLEADER run is one undo step.

## Chains and text editing (Q67)

- **DIMCONTINUE** (DCO) continues from the last linear or aligned dimension: the new one starts at the previous
  second extension line and shares its dimension line. Keep picking points; Enter ends. **Select** chooses another
  base dimension.
- **DIMBASELINE** (DBA) starts every new dimension at the base dimension's first extension line and stacks each one
  further out, by 2.5 text heights (scaled by the style's overall scale).
- **DIMTEDIT** (DTE) moves the text of a picked dimension to a new point; **Home** puts it back at its automatic place.
