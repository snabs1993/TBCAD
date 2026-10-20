#pragma once

#include <optional>
#include <string_view>
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

// A gradient fill as colour bands (HatchData::gradient). Linear, cylinder and curved kinds cut
// the area into `bands` strips across the gradient direction; spherical and hemispherical kinds
// into nested discs around their centre (outermost first). Each band carries the rings of its
// part of the area (even-odd, holes included) and the colour parameter `t` in 0..1 (0 = colour
// 1, 1 = colour 2). Empty when the hatch has no gradient. Bands overlap or cover the ones before
// them; paint them in order.
struct GradientBand {
    std::vector<std::vector<Vec2>> rings{};
    double t = 0;
};
std::vector<GradientBand> gradientBands(const HatchData& hatch, double tolerance,
                                        std::size_t bands = 64);

// AutoCAD's gradient names (DXF 470): LINEAR, CYLINDER, INVCYLINDER, SPHERICAL, INVSPHERICAL,
// HEMISPHERICAL, INVHEMISPHERICAL, CURVED, INVCURVED. The parse ignores case.
const char* gradientName(GradientKind kind);
std::optional<GradientKind> gradientKindFromName(std::string_view name);
// The second colour of a one-colour gradient: `c` at the lightness `tint` (AutoCAD's
// GFCLRLUM): 0 black, 0.5 the colour itself, 1 white.
Color tintedColor(Color c, double tint);

// The separate areas of a hatch (several areas picked in one HATCH make one hatch): one
// HatchData per area, each with its outer loop and the islands inside it, the rest of the hatch
// (pattern, gradient, sources) copied. A hatch of one area comes back as it is.
std::vector<HatchData> separateAreas(const HatchData& hatch);

// TRIM on a hatch: the cutting edges and the hatch's own outline split the hatched area into
// pieces, and the piece under `pick` is taken away. The result keeps the pattern or gradient and
// is no longer associative. Errors: `pick` is not on the hatched area; no edge crosses the hatch
// there (the whole hatch would go).
Result<HatchData> trimHatch(const HatchData& hatch, Vec2 pick,
                            const std::vector<const Entity*>& edges);

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
