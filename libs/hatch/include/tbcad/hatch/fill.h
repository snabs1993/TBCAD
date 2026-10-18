#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "tbcad/core/entities.h"
#include "tbcad/geom/curve.h"
#include "tbcad/hatch/pattern.h"

namespace tbcad::hatch {

// Curves of one hatch loop, in loop order. Arcs and ellipses are returned counter-clockwise,
// whatever direction the loop travels them; splines and polyline edges are expanded.
std::vector<geom::CurveRef> loopCurves(const HatchLoop& loop);

struct FillOptions {
    // Chord tolerance (mm) used when flattening arcs for a solid fill.
    double tolerance = 0.01;
    // Size of a screen pixel in mm; 0 = unknown (the density guard is then off). A pattern whose
    // line spacing is below `minSpacingPx` pixels is drawn as a solid tone instead.
    double pixelSizeMm = 0;
    double minSpacingPx = 2.0;
    // More dash segments (dots count too) than this make the fill fall back to a solid tone.
    std::size_t maxSegments = 50000;
    // More pattern lines than this (summed over the families) also fall back.
    std::size_t maxLines = 400000;
};

struct FillResult {
    enum class Mode {
        Solid,      // `rings` is the whole answer: a solid fill (even-odd)
        Lines,      // `segments` and `dots` hold the pattern
        SolidTone,  // the pattern was too dense to draw: `rings` as a stand-in, see `reason`
    };
    Mode mode = Mode::Solid;
    std::vector<std::vector<Vec2>> rings{};
    std::vector<std::pair<Vec2, Vec2>> segments{};
    std::vector<Vec2> dots{};
    // Set for SolidTone: "too dense" (spacing under the pixel limit) or "too many segments".
    std::string reason{};
};

// Fills the area of `hatch` (all loops, even-odd) with `pattern`, using the hatch's scale, angle
// and origin. A solid pattern, or HatchData::solid, gives Mode::Solid.
//
// Pattern lines are intersected with the loop edges exactly (arcs are not flattened); only the
// rings of the solid fill are flattened, at `options.tolerance`.
FillResult generateFill(const HatchData& hatch, const Pattern& pattern,
                        const FillOptions& options = {});

// Flattened rings of the loops (each closed implicitly).
std::vector<std::vector<Vec2>> loopRings(const HatchData& hatch, double tolerance);

}  // namespace tbcad::hatch
