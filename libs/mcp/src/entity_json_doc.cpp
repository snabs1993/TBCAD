#include "tbcad/mcp/entity_json.h"

namespace tbcad::mcp {

const std::string& entityJsonDoc() {
    static const std::string doc = R"MD(# TBCAD entity JSON

Units: lengths are millimetres, angles are radians. Anywhere an angle is expected you may write
`{"deg": 90}`; anywhere a length is expected you may write `{"value": 3, "unit": "m"}` (units: mm,
cm, m, in, ft). Points are `[x, y]` (or `{"x": 1, "y": 2}`). Output is always mm and radians.
This holds in every tool and in every drawing: a drawing set to show metres still takes plain
numbers as millimetres (a 177 m long plan is 177000), and get_drawing_info reports `displayUnit`.

Every entity has:

| field | meaning |
|---|---|
| `id` | decimal string (64-bit safe); ignored when writing |
| `kind` | line, circle, arc, polyline, point, ellipse, spline, text, mtext, hatch, insert, dimension, leader, table, ray, mline, mleader, proxy |
| `layer` | full layer path, sub-layers joined with `~` (`S-FLOOR~edge`) |
| `level` | level name; only written when the drawing has more than one level; when writing, the current level is used if omitted |
| `color` | `"ByLayer"`, `"ByBlock"`, an ACI number 1..255, or `"#RRGGBB"` |
| `linetype` | linetype name, `"ByLayer"` or `"ByBlock"` |
| `lineweight` | mm number, `"ByLayer"`, `"ByBlock"` or `"Default"` |
| `linetypeScale`, `visible` | only present when not 1 / not true |

Kind fields:

- **line**: `start`, `end`
- **circle**: `center`, `radius`
- **arc**: `center`, `radius`, `startAngle`, `endAngle` (counter-clockwise from start to end)
- **polyline**: `vertices` (`[[x,y],...]` or `[{x,y,bulge,startWidth,endWidth},...]`), `closed`.
  `bulge = tan(sweep/4)`, positive = counter-clockwise, belongs to the segment starting at that vertex.
- **point**: `position`
- **ellipse**: `center`, `majorAxis` (vector from the centre to the end of the major axis), `ratio` (minor/major), optional `startParam`, `endParam`
- **spline**: `degree`, `controlPoints`, optional `fitPoints`, `weights`, `closed`
- **text**: `position`, `height`, `value`, optional `rotation`, `style`, `hAlign` (left, center, right, aligned, middle, fit), `vAlign` (baseline, bottom, middle, top), `alignPoint`, `widthFactor`, `oblique`, and `attdef` `{"tag", "prompt", "default", "invisible", "constant", "verify", "preset"}` which makes the text an attribute definition (put it in a block with `create_block`; inserts then take `attributes`)
- **mtext**: `position`, `height`, `width`, `value`, `attachment` (topLeft ... bottomRight), optional `rotation`, `style`
- **hatch**: `loops`, `solid`, and for patterns `pattern`, `scale`, `angle`, optional `patternOrigin`. `fill` is the solid colour (default ByLayer). `gradient` `{"type": "linear"|"cylinder", "color1", "color2", "angle"}` makes it a gradient fill (colours as for `color`). `associativeWith` lists the boundary entity ids the hatch follows (read only). When writing, a loop is `[[x,y],...]` or `{"vertices": [...]}` or `{"edges": [{"type":"line"|"arc"|"polyline",...}]}`.
- **insert**: `block` (name), `position`, `scale` (number or `[sx, sy]`), `rotation`. Optional `attributes` `{"TAG": "value"}` (values of the block's attribute definitions), `dynamicValues` (numbers, in the order of the block's dynamic parameters) and `array`, which turns the insert into an associative array:
  `{"kind":"rectangular","rows","columns","rowSpacing","columnSpacing","angle"}`,
  `{"kind":"polar","center","count","fillAngle","rotateItems","reference"}` or
  `{"kind":"path","path":[vertices],"pathClosed","count","pathSpacing","rotateItems","reference"}`.
- **dimension**: `dimKind` is `linear`, `aligned`, `radius`, `diameter`, `angular` (two lines) or `angular3`. Writing takes one of
  `linear`/`aligned`: `start`, `end`, `dimLine` (a point on the dimension line); linear also `rotation` (0 = horizontal, pi/2 = vertical);
  `radius`/`diameter`: `center`, `radius`, optional `angle` (where the dimension sits on the circle);
  `angular`: `lines` `[[a, b], [c, d]]` and `arcPoint` (a point on the dimension arc);
  `angular3`: `first`, `second`, `vertex`, `arcPoint`;
  or the raw `definitionPoints` that reading returns. Optional `text` (override; `<>` stands for the measured value), `textPosition` (fixes the text there unless `textPositionFixed` is false; reading returns both), `dimStyle` (name). `measurement` and the text position are computed. Baseline and continued chains are made with `run_command` (DIMBASELINE, DIMCONTINUE) or by adding more linear dimensions that share a point.
- **leader**: `vertices` (at least two, the arrow end first), optional `text`, `textPosition`, `dimStyle`.
- **ray**, **xline**: `origin`, `direction` (normalised when written); a ray starts at the origin, an xline (`kind: "xline"` when writing, or `infinite: true`) runs both ways. Reads back as kind `ray` with `infinite`.
- **mleader**: `lines` (each a list of points, the arrow end first), `landing` (where the lines meet), `contentPosition` (end of the dogleg), `dogleg`, `content` (`text`, `block` or `none`), `text`, `textHeight`, `block` with `blockScale` and `blockRotation`, `dimStyle`.
- **mline**: `vertices`, `style` (MLSTYLE name), `scale`, `justification` (0 top, 1 zero, 2 bottom), `closed`.
- **table**: `origin` (top-left), `columnWidths`, `rowHeights`, `cells` (row-major strings, MText markup allowed), `rotation`, `textHeight`, `margin`, `titleRow`.
- **proxy**: an imported entity of an unknown type; read only.

Writing: `add_entities` accepts line, circle, arc, polyline, point, ellipse, spline, text, mtext,
hatch, insert, dimension, leader, mleader, ray, xline, mline and table. `modify_entities` `set` takes any of the fields above for the entity's kind.
)MD";
    return doc;
}

}  // namespace tbcad::mcp
