#pragma once

#include <string>
#include <utility>
#include <vector>

#include "tbcad/core/core.h"

namespace tbcad::transform {

// STRETCH by a crossing window: the entities as they look after moving every vertex inside
// `window` by `delta`. Lines, arcs, polylines, splines, multilines, leaders and hatch outlines
// move only the vertices inside the window; if every vertex is inside the entity simply moves.
// Entities with one anchor (circles, ellipses, text, inserts, tables, rays, construction lines,
// points) move whole when the anchor is inside; dimensions move the definition points inside.
// An associative hatch then follows its boundary when the transaction commits. Entities the window
// does not touch are left out. Each entry is the source id and its new state; `error` receives the
// first failure.
std::vector<std::pair<EntityId, Entity>> stretchByWindow(const Document& doc,
                                                         const std::vector<EntityId>& ids,
                                                         const BBox2& window, Vec2 delta,
                                                         std::string* error = nullptr);

}  // namespace tbcad::transform

namespace tbcad::transform {

// One crossing area of STRETCH: a window, or a polygon when `polygon` has points.
struct StretchRegion {
    BBox2 window{};
    std::vector<Vec2> polygon{};
};

// STRETCH with several crossing windows and polygons, as AutoCAD collects them: a vertex or anchor
// moves when it lies in any of them. Objects whose box lies inside one window move whole.
std::vector<std::pair<EntityId, Entity>> stretchByRegions(const Document& doc,
                                                          const std::vector<EntityId>& ids,
                                                          const std::vector<StretchRegion>& regions,
                                                          Vec2 delta, std::string* error = nullptr);

// The same for a crossing polygon (CPolygon): a vertex or anchor is inside when it lies in the
// polygon. Fewer than three points select nothing.
std::vector<std::pair<EntityId, Entity>> stretchByPolygon(const Document& doc,
                                                          const std::vector<EntityId>& ids,
                                                          const std::vector<Vec2>& polygon,
                                                          Vec2 delta, std::string* error = nullptr);

}  // namespace tbcad::transform
