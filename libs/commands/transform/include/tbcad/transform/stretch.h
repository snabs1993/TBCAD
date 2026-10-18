#pragma once

#include <string>
#include <utility>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::transform {

// STRETCH by a crossing window: the entities as they look after moving every vertex inside
// `window` by `delta`. Lines, arcs, polylines, splines and leaders move only the vertices inside
// the window; if every vertex is inside the entity simply moves. Entities with one anchor
// (circles, ellipses, text, inserts, points) move whole when the anchor is inside; dimensions and
// hatches move whole when their bounding box is inside. Entities the window does not touch are
// left out. Each entry is the source id and its new state; `error` receives the first failure.
std::vector<std::pair<EntityId, Entity>> stretchByWindow(const Document& doc,
                                                         const std::vector<EntityId>& ids,
                                                         const BBox2& window, Vec2 delta,
                                                         std::string* error = nullptr);

}  // namespace tbcad::transform

namespace tbcad::transform {

// The same for a crossing polygon (CPolygon): a vertex or anchor is inside when it lies in the
// polygon. Fewer than three points select nothing.
std::vector<std::pair<EntityId, Entity>> stretchByPolygon(const Document& doc,
                                                          const std::vector<EntityId>& ids,
                                                          const std::vector<Vec2>& polygon,
                                                          Vec2 delta, std::string* error = nullptr);

}  // namespace tbcad::transform
