#pragma once

#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::blocks {

// Whether `p` is inside the closed polygon (even-odd rule; boundary counts as inside).
bool insidePolygon(const std::vector<Vec2>& polygon, Vec2 p);

// The part of `in` inside the closed polygon. Polylines are cut where they cross the boundary,
// fills are cut with the polygon (a concave boundary is cut as triangles), points are kept when
// inside. A fill the boundary does not reach is kept whole. With `invert` the part outside the
// polygon is kept instead.
DisplayList clipDisplay(const DisplayList& in, const std::vector<Vec2>& polygon,
                        bool invert = false);

}  // namespace tbcad::blocks
