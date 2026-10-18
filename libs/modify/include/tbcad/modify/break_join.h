#pragma once

#include <cstddef>
#include <vector>

#include "tbcad/modify/common.h"

namespace tbcad::modify {

// Removes the part of the entity between the points p1 and p2 (each projected onto the entity).
// Lines, arcs, open ellipses, splines and open polylines lose the part between the two points
// (in either order); a closed polyline is opened and loses the part between them. A circle (or
// closed ellipse) loses the arc running counter-clockwise from p1 to p2, so what remains is the arc
// from p2 counter-clockwise to p1. If a point lies at or beyond an end, that end is cut off.
Result<EntityEdit> breakEntity(const Entity& e, Vec2 p1, Vec2 p2);
// Splits the entity in two at p (BREAKATPOINT). A closed polyline is opened at p. Circles and other
// closed curves cannot be split at one point.
Result<EntityEdit> breakAtPoint(const Entity& e, Vec2 p);

struct JoinResult {
    std::vector<Entity> created{};
    std::vector<std::size_t> consumed{};  // indices into the input list, to be erased
};

// Joins entities that touch (end points within `tolerance`) or overlap:
//   collinear lines            -> one line (overlapping or touching)
//   arcs on one circle         -> one arc, or a circle when they close up
//   chains of lines, arcs and open polylines -> one polyline (closed when the chain closes)
// Other entities, and entities that find no partner, are left out of `consumed`. The results take
// the properties of the first entity of each group in the list. With `bridgeGaps`, collinear
// lines and arcs of one circle join even when there is a gap between them (the gap is filled).
JoinResult joinEntities(const std::vector<const Entity*>& entities, double tolerance = 1e-4,
                        bool bridgeGaps = false);

}  // namespace tbcad::modify
