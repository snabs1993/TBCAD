#pragma once

#include <cstddef>
#include <vector>

#include "tbcad/core/entities.h"
#include "tbcad/core/math.h"
#include "tbcad/core/result.h"

namespace tbcad::hatch {

// An entity offered to the boundary finder. `entity` must stay alive during the call.
struct Candidate {
    EntityId id{};
    const Entity* entity = nullptr;
};

struct BoundaryOptions {
    // More graph edges than this (after every curve was split at its crossings) aborts the search
    // with BoundaryError::TooManyEdges.
    std::size_t maxEdges = 20000;
    // Largest gap between two end points that still counts as touching. v1 only supports 0, which
    // means "end points must meet within tol::kLinear" (the default). Reserved for later.
    double gapTolerance = 0;
    // Chord tolerance (mm) used to decide containment and compare areas. Does not change the
    // geometry that is returned.
    double flattenTolerance = 1e-3;
};

struct Boundary {
    // loops[0] is the outer boundary (counter-clockwise, `outer == true`); the rest are loops
    // strictly inside it. Loops at odd nesting depth are holes (`outer == false`), at even depth
    // (2, 4, ...) they are filled again: Normal island style. Every loop is counter-clockwise.
    std::vector<HatchLoop> loops{};
    // Ids of the entities whose edges form the outer loop (each once, in loop order).
    std::vector<EntityId> sources{};
    // Size of the planar graph that was searched, for diagnostics and tests.
    std::size_t graphEdges = 0;
};

enum class BoundaryError {
    NoClosedBoundary,  // no closed region contains the point
    TooManyEdges,      // zoom in: more than BoundaryOptions::maxEdges edges
    NoGeometry,        // no candidate has curve geometry
};

// Message for the command line, e.g. "Too many objects in view; zoom in and try again."
const char* boundaryErrorText(BoundaryError e);

// Pick-point mode. Splits all candidate curves at their mutual intersections, builds the planar
// graph, walks its faces, and returns the smallest enclosed region that contains `pick` together
// with the loops inside it. Open ends and overlaps are tolerated: a dangling line inside a
// region does not disturb it. Splines that get split are returned as polyline edges.
//
// The Error's message is boundaryErrorText(); its code is Generic for NoClosedBoundary and
// OutOfRange for TooManyEdges.
Result<Boundary> findBoundary(const std::vector<Candidate>& candidates, Vec2 pick,
                              const BoundaryOptions& options = {});

// Select-objects mode. Works on the selected entities only: every region they enclose is found
// the same way as in findBoundary. Each outermost region, together with whatever lies inside it,
// becomes one Boundary (two separate rectangles give two). Closed entities (circles, closed
// polylines, ...) simply become loops, and open entities whose end points meet close up too.
// Boundaries are sorted by the position of their lower-left corner (x, then y). Fails when
// nothing closed was found.
Result<std::vector<Boundary>> boundaryFromObjects(const std::vector<Candidate>& objects,
                                                  const BoundaryOptions& options = {});

}  // namespace tbcad::hatch
