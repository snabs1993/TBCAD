# Regions and freehand drawing

A region here is a closed polyline (outer boundaries, plus one more polyline for each hole). `REGION` converts closed objects (closed polylines, circles, full ellipses, closed splines) into such polylines. Arcs are flattened to polygons 0.005 units fine. DXF has no plain 2D region, so regions are saved as polylines and round-trip exactly.

| Command | Alias | What it does |
|---|---|---|
| `REGION` | `REG` | Closed objects to closed polylines |
| `UNION` | `UNI` | Merge the selected closed objects into one outline |
| `SUBTRACT` | `SU` | Select what to cut from, then what to cut out |
| `INTERSECT` | `IN` | Keep only the area all selected objects share |
| `BOUNDARY` | `BO`, `BPOLY` | Closed polyline around a point (existing); the `Islands` keyword picks Normal (every closed shape inside), Outer (first level only) or Ignore (outline alone), kept in `boundary.islands` |
| `SKETCH` | `SK` | Freehand: click to put the pen down, move to draw, click to lift, Enter to keep. `Increment` sets the spacing between recorded points |

The boolean code is `geom::boolean(Region, Region, BoolOp)` in `libs/geom/include/tbcad/geom/polybool.h`; use it for any outline merging.
