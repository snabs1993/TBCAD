#include "tbcad/mcp/entity_json.h"

namespace tbcad::mcp {

const std::string& entityJsonDoc() {
    static const std::string doc = R"MD(# TBCAD entity JSON

Units: lengths are millimetres, angles are radians. Anywhere an angle is expected you may write
`{"deg": 90}`; anywhere a length is expected you may write `{"value": 3, "unit": "m"}` (units: mm,
cm, m, in, ft). Points are `[x, y]` (or `{"x": 1, "y": 2}`). Output is always mm and radians.

Every entity has:

| field | meaning |
|---|---|
| `id` | decimal string (64-bit safe); ignored when writing |
| `kind` | line, circle, arc, polyline, point, ellipse, spline, text, mtext, hatch, insert, dimension, leader, proxy |
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
- **text**: `position`, `height`, `value`, optional `rotation`, `style`, `hAlign` (left, center, right, aligned, middle, fit), `vAlign` (baseline, bottom, middle, top), `alignPoint`, `widthFactor`, `oblique`
- **mtext**: `position`, `height`, `width`, `value`, `attachment` (topLeft ... bottomRight), optional `rotation`, `style`
- **hatch**: `loops`, `solid`, and for patterns `pattern`, `scale`, `angle`. When writing, a loop is `[[x,y],...]` or `{"vertices": [...]}` or `{"edges": [{"type":"line"|"arc"|"polyline",...}]}`.
- **insert**: `block` (name), `position`, `scale` (number or `[sx, sy]`), `rotation`
- **dimension**, **leader**: read only here; create them with `run_command` (DIMLINEAR, DIMALIGNED, DIMRADIUS, LEADER ...).
- **proxy**: an imported entity of an unknown type; read only.

Writing: `add_entities` accepts line, circle, arc, polyline, point, ellipse, spline, text, mtext,
hatch and insert. `modify_entities` `set` takes any of the fields above for the entity's kind.
)MD";
    return doc;
}

}  // namespace tbcad::mcp
