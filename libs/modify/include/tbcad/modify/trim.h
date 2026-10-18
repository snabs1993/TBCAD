#pragma once

#include <vector>

#include "tbcad/modify/common.h"

namespace tbcad::modify {

struct TrimOptions {
    // Cutting / boundary edges are treated as if they ran on: lines to infinity, arcs and circles
    // to the full circle, ellipses to the full ellipse (AutoCAD's "Edge: Extend").
    bool extendEdges = false;
};

// Trims the part of `target` under `pick` away, back to the nearest cutting edges on both sides.
// Works on lines, arcs, circles, ellipses, splines, rays, construction lines and polylines. A
// circle becomes an arc, a ray keeps a line towards its start, a closed
// polyline is opened at the cut. The edges are the entities that cut (the target itself must not
// be among them); entities without curve geometry are ignored. With only one cutting edge on one
// side of the pick, everything from the pick to that end is removed. Errors: no edge crosses the
// object on either side of the pick; a circle or closed curve with fewer than two cuts.
Result<EntityEdit> trimEntity(const Entity& target, Vec2 pick,
                              const std::vector<const Entity*>& edges,
                              const TrimOptions& options = {});

// Extends the end of `target` nearest to `pick` to the nearest boundary edge ahead of it. Works
// on lines, arcs and open polylines (the first / last segment). Errors: no boundary ahead.
Result<EntityEdit> extendEntity(const Entity& target, Vec2 pick,
                                const std::vector<const Entity*>& edges,
                                const TrimOptions& options = {});

}  // namespace tbcad::modify
