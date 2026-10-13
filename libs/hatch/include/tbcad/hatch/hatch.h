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

// A gradient fill as colour bands (HatchData::gradient): the hatch area cut into `bands` strips
// across the gradient direction. Each band carries the rings of its part of the area (even-odd,
// holes included) and the colour parameter `t` in 0..1 (0 = colour 1, 1 = colour 2). Empty when
// the hatch has no gradient. Bands overlap their successor slightly; paint them in order.
struct GradientBand {
    std::vector<std::vector<Vec2>> rings{};
    double t = 0;
};
std::vector<GradientBand> gradientBands(const HatchData& hatch, double tolerance,
                                        std::size_t bands = 64);

// Associative hatches. For every hatch whose `sourceBoundary` contains an entity touched by `tx`
// (Transaction::touchedEntities), the boundary is found again from the source entities and the
// hatch's loops are replaced. A hatch whose source was erased, or whose sources no longer enclose
// a region, becomes non-associative (empty sourceBoundary) and keeps its last loops. Hatches whose
// loops would not change are not written. Meant to run as a commit hook (see
// cmd::Registry::addCommitHook) so the update joins the command's undo step.
// Returns the number of hatches that were rewritten or detached.
std::size_t updateAssociativeHatches(Document& doc, Transaction& tx,
                                     const BoundaryOptions& options = {});

}  // namespace tbcad::hatch
