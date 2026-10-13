#pragma once

#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::blocks {

// Whether `p` is inside the closed polygon (even-odd rule; boundary counts as inside).
bool insidePolygon(const std::vector<Vec2>& polygon, Vec2 p);

// The part of `in` inside the closed polygon. Polylines are cut where they cross the boundary,
// fills are cut with the polygon (exact for a convex boundary; a concave one can leave
// zero-width slivers along its edges), points are kept when inside.
DisplayList clipDisplay(const DisplayList& in, const std::vector<Vec2>& polygon);

}  // namespace tbcad::blocks
