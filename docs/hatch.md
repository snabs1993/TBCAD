# Hatch: boundaries, patterns, fills, HATCH / HATCHEDIT

Library `libs/hatch` (`tbcad::hatch`, depends on `tbcad::core` and `tbcad::geom`), command module
`libs/commands/hatch` (`tbcad::hatchcmd`), patterns in `assets/patterns/tbcad.patterns`.
Include `tbcad/hatch/hatch.h` for everything.

## Boundary detection (`boundary.h`)

```cpp
std::vector<hatch::Candidate> cands;            // entities that may bound the hatch
auto r = hatch::findBoundary(cands, pickPoint); // Result<Boundary>
```

* The caller decides which entities take part (the HATCH command passes the visible, model-space
  entities inside the current view). Anything without curve geometry is ignored.
* All curves are split at their mutual intersections (`geom::intersect`, so arcs, ellipses and
  splines are handled exactly), equal end points become one graph node (1e-5 mm), duplicate and
  overlapping edges are dropped, and the planar graph's faces are walked with the smallest-turn
  rule. Lines that leave a node in the same direction (tangent arcs) are ordered by curvature.
* Bridges and dangling lines are traversed twice by a face walk and disappear as zero-area cycles,
  so an antenna inside a region, or a circle joined to the frame by a line, behaves.
* The answer is the **smallest** loop that contains the point, plus every loop strictly inside it
  (`Boundary::loops[1..]`). Nesting depth decides the flag: depth 1 is a hole (`outer == false`),
  depth 2 is filled again (`outer == true`), and so on: the Normal island style. All loops run
  counter-clockwise. A neighbour that shares an edge is not inside. Splines that had to be split
  come back as `HatchPolylineEdge`s (1e-4 mm chord); whole splines stay splines.
* Guard: more than `BoundaryOptions::maxEdges` (20,000) graph edges → `Error{OutOfRange}`
  "Too many objects in view to find a boundary; zoom in and try again." The check also stops the
  search early when the number of crossings alone is far over the limit.
* **Gap tolerance** (`BoundaryOptions::gapTolerance`, mm, default 0): a free end that stops short of
  another curve (its end or its side) by at most the tolerance is joined to it with a connecting
  line. The connector is a normal line edge of the loop but has no source entity, so
  `Boundary::sources` lists real entities only. One extra pass over all pieces per free end.
* `Boundary::sources` lists the outer loop's entities first, then the islands'. This is what an
  associative hatch stores in `HatchData::sourceBoundary`.
* `BoundaryFinder` builds the graph once and answers many picks (`find(pick)`, `outermost()`); it is
  what the hover preview uses. The entities handed to it must stay unchanged while it is used.
* Edge encoding in `HatchData`: `HatchArcEdge` / `HatchEllipseEdge` travel from `startAngle` to
  `endAngle` counter-clockwise when `ccw`, clockwise otherwise; equal angles mean a full turn.
* `boundaryFromObjects(objects)` is the select-objects mode: the regions enclosed by the chosen
  entities only, one `Boundary` per outermost region (closed entities become loops directly; loose
  lines that close up work too).

Measured (Release, GCC 13, `libs/hatch/tests/perf_tests.cpp`): boundary detection among 5,000
visible entities (2,500 squares each holding a circle, a frame and two loose lines; 12,560 graph
edges) takes about **81 ms**; the same test under Debug takes about 390 ms, which is why the test only
enforces the 300 ms budget when `NDEBUG` is defined.

## Patterns (`pattern.h`)

`assets/patterns/tbcad.patterns` is our own format, written from scratch (see the comment at the
top of the file): `[NAME]` sections with `description`, `solid` and `line = angle | origin | delta |
dashes`. The file is compiled into the library by `libs/hatch/CMakeLists.txt`, so nothing is read
at run time. Shipped: SOLID, LINE45, LINE0, CROSS45, GRID, BRICK, DOTS, SAND (eight dot families
from a fixed random seed, 20261013) and EARTH.

A family is a set of parallel lines: line *k* passes through `origin + k·(delta.x·u + delta.y·n)`
(u = the family direction, n = perp(u)); the dash cycle starts at that point; negative numbers are
gaps and 0 is a dot. All lengths are mm at scale 1. `HatchData::patternScale`, `patternAngle` and
`patternOrigin` transform the whole pattern. `PatternRegistry::add` lets the application add user
patterns (they shadow built-in ones); `parsePatterns(text)` reads the file format.

## Fill generation (`fill.h`)

`generateFill(hatchData, pattern, options)` → `FillResult`:

* `Solid`: `rings` (all loops flattened at `options.tolerance`; even-odd).
* `Lines`: `segments` (one per drawn dash) and `dots`. Each family's infinite lines are
  intersected with the loop edges **exactly** (no flattening of arcs); the sorted crossing
  parameters are paired even-odd, and the dash cycle is applied to each inside interval. A line
  through a vertex, along an edge or tangent to an arc is nudged sideways by 1e-5 of the spacing so
  that the pairing stays valid.
* `SolidTone`: the stand-in for patterns that cannot be drawn. Triggers: line spacing under
  `minSpacingPx` pixels (needs `pixelSizeMm`), more than `maxLines` (400,000) lines, or more than
  `maxSegments` (**200,000**) dash segments. `reason` says which. The display generator draws the
  rings with the hatch colour at reduced alpha.

## Display, bounds, explode (`hatch.h`)

* `registerHatchDisplay()` plugs the generator into `geom::DisplayRegistry` for `EntityKind::Hatch`
  (the command module calls it). Colour: the entity's resolved colour, or `HatchData::fill` when
  that is not ByLayer. Pattern lines are continuous and thin (lineweight 0) unless the hatch has a
  lineweight of its own.
* `makeHatchEntity(boundary, pattern, scale, angle, origin)` builds the entity.
* `explodeHatch(doc, hatch)` is the hook for the EXPLODE command (W5-3): Line and Point entities for a
  pattern hatch, the boundary edges for a solid one. Refuses a pattern that is too dense.

## Gradient fills and the pattern library

* `HatchData::gradient` (`GradientData`: kind Linear or Cylinder, two colours, angle) turns a hatch
  into a colour gradient. The hatch is solid (`solid == true`, pattern SOLID). The display
  generator cuts the area into 64 bands across the gradient direction (`gradientBands`, rings
  clipped per band, even-odd and holes kept) and fills each with the blended colour; bands overlap
  their successor by a quarter band so no seams show. Cylinder runs colour 1 -> 2 -> 1. Saved
  in `.tbcad` as a few bytes after the loops of the hatch record (older readers skip them); DXF
  export writes a gradient hatch as a plain solid hatch.
* `assets/patterns/tbcad.patterns` has 34 patterns, all of our own design: LINE90, LINE135, LINE30,
  LINE60, LINE45F/W/D, DASHES, GRIDFINE, GRIDWIDE, DIAMOND, TRIANGLE, STEEL, TICKS, WOOD, MESH,
  BLOCK, STONE, TILEDOT, PLASTER, DOTSTAG, CROSSES, ZIGZAG, and the seeded random CONCRETE and
  GRAVEL, next to the first nine. ZIGZAG's numbers were derived so that the 45 degree and 315
  degree dash families meet at the zigzag corners (6 mm period, 3 mm high, rows 8 mm apart).

## Commands (`libs/commands/hatch`)

* **HATCH [H]**: `Pick internal point (LINE45, scale 1, angle 0) or [Objects/Pattern/Scale/Angle/Base/Gap/Link/Undo]`.
  Each pick adds a hatch (islands included). `Objects` hatches the selected shapes, `Undo` removes the
  last hatch of this run, Esc keeps what was made. The boundary search sees the model-space entities
  that are visible (entity visible, layer on and not frozen) and inside the current view; without a
  viewport (scripts) it sees all of them. The chosen pattern, scale, angle and base are remembered in
  the host settings (`hatch.pattern`, `hatch.scale`, `hatch.angle` in degrees, `hatch.base`, `hatch.gap`,
  `hatch.associative`). `Gap` sets the gap tolerance (0 = off); `Link` toggles whether new hatches
  are associative. While the command waits for a point, the region under the cursor is previewed
  (outline plus a light tint), from a boundary graph that is cached per drawing revision, view and
  gap. Candidates come from `IHost::entitiesInWindow` when the host has a spatial index, otherwise
  from a scan of the document.
* **Associative hatches.** A commit hook registered by the module calls
  `hatch::updateAssociativeHatches` at the end of every command that changed something. A hatch
  whose `sourceBoundary` contains a touched entity is rebuilt from its source entities (the
  outermost region that best overlaps the old outer loop) in the same transaction, so the update
  is part of the command's undo step. If a source was erased or the sources no longer enclose a
  region, the hatch keeps its last loops and becomes non-associative. Unchanged geometry is not
  rewritten. Limitation: a hatch that was closed with a gap is rebuilt without a gap tolerance, so it
  detaches once its sources move apart.
* **GRADIENT [GR]**: the same command with a gradient instead of a pattern:
  `Pick internal point (linear 30,60,200 to 190,220,255, angle 0) or [Objects/Kind/First/Second/Angle/Gap/Link/Undo]`.
  `Kind` Linear or Cylinder, `First` and `Second` colours (index 1-255, `r,g,b` or `#rrggbb`), `Angle`
  the direction of the colour change. Settings `hatch.gradient.kind`, `hatch.gradient.color1`,
  `hatch.gradient.color2`. Preview, Gap and Link work as in HATCH. HATCHEDIT with a pattern removes
  the gradient.
* **HATCHEDIT [HE]**: select hatches (the current selection is used first), then
  `[Pattern/Scale/Angle/Base/Done]`. All selected hatches change in one transaction, so editing
  1,000 of them is one undo step. Esc throws the edits away. The property section and the panel call it
  as `HATCHEDIT P LINE45 S 2 A 45 D`.
* UI (application builds only): menu entries Draw/Hatch and Modify/Edit Hatch, a *Hatch* panel
  (pattern list with thumbnails, scale, angle, origin, "Pick internal point", "Apply to selected
  hatches") and a Hatch section in the properties panel (pattern, scale, angle, origin).
