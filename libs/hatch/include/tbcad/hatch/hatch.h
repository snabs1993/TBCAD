#pragma once

#include <vector>

#include "tbcad/core/document.h"
#include "tbcad/core/entities.h"
#include "tbcad/core/result.h"
#include "tbcad/hatch/boundary.h"
#include "tbcad/hatch/fill.h"
#include "tbcad/hatch/pattern.h"

namespace tbcad::hatch {

// Registers the Hatch display and bounds generator with geom::DisplayRegistry. Safe to call more
// than once. The command module calls it from its registerCommands.
void registerHatchDisplay();

// Bounding box of all loops (curve bounds, not flattened).
BBox2 hatchBounds(const HatchData& hatch);

// A new hatch entity for `boundary`: loops, pattern and parameters are filled in; everything in
// EntityCommon is left at its defaults (the document applies the current layer on add).
Entity makeHatchEntity(const Boundary& boundary, const Pattern& pattern, double scale,
                       double angleRadians, Vec2 origin);

// Explode hook for W5-3. A pattern hatch becomes one Line (and Point for dots) entity per dash
// segment; a solid hatch becomes its boundary edges (Line, Arc, Circle, Ellipse, Spline, Polyline
// entities). The result carries the hatch's layer, colour (the hatch's fill colour when it has
// one), linetype and lineweight. `tolerance` is the flattening tolerance of the solid fill.
Result<std::vector<Entity>> explodeHatch(const Document& doc, const Entity& hatch,
                                         double tolerance = 0.01);

}  // namespace tbcad::hatch
