#pragma once

#include <vector>

#include "tbcad/modify/common.h"

namespace tbcad::modify {

struct OffsetOptions {
    // Convex corners of a polyline offset get a circular arc (true) or a sharp mitre (false).
    // Concave corners are always sharp.
    bool roundCorners = true;
    // Maximum deviation of an ellipse / spline offset (approximated by a spline), in mm.
    double tolerance = 0.01;
};

// Side of the entity on which `p` lies relative to the direction of travel: +1 left, -1 right,
// 0 on the entity (or unsupported kind). Left is the inside of a counter-clockwise circle, arc,
// ellipse or closed polyline.
int sideOfEntity(const Entity& e, Vec2 p);

// Offsets a line, circle, arc, polyline, ellipse or spline. `signedDistance` > 0 offsets to the
// left of the direction of travel, < 0 to the right. The results are new entities with the
// source's properties: usually one, several when a polyline splits (e.g. a concave offset that
// pinches), none (an error) when nothing is left.
Result<std::vector<Entity>> offsetEntity(const Entity& e, double signedDistance,
                                         const OffsetOptions& options = {});
// Offsets by `distance` (>= 0) to the side of `sidePoint`.
Result<std::vector<Entity>> offsetToward(const Entity& e, Vec2 sidePoint, double distance,
                                         const OffsetOptions& options = {});
// Offsets to the curve through `through`; the distance is that point's distance to the entity.
Result<std::vector<Entity>> offsetThrough(const Entity& e, Vec2 through,
                                          const OffsetOptions& options = {});

}  // namespace tbcad::modify
