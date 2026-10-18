#pragma once

#include <optional>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/index/spatial_index.h"

namespace tbcad::ui {

// True when the user may pick the entity: its own flag is on, and the layer, resolved through
// Document::effective() so sub-layers follow their parents, is visible (on and not frozen) and not
// locked. Entities inside block definitions are never selectable, except the objects on a layout
// sheet.
bool isSelectable(const Document& doc, const Entity& e);
// True when the entity is drawn at all (locked layers are drawn, faded).
bool isVisible(const Document& doc, const Entity& e);

// The selectable entity nearest to `p` within `apertureMm` (ties go to the one drawn on top, i.e.
// the larger id). Uses the spatial index for candidates and geom::distance for the decision.
std::optional<EntityId> pickEntity(const Document& doc, const index::SpatialIndex& index, Vec2 p,
                                   double apertureMm);

// Every selectable entity within `apertureMm` of `p`, nearest first (ties: larger id first).
std::vector<EntityId> pickAllAt(const Document& doc, const index::SpatialIndex& index, Vec2 p,
                                double apertureMm);
// Selection cycling: the entity after `current` in `stack` (wrapping round); the first one when
// `current` is not in it. Nullopt for an empty stack.
std::optional<EntityId> cycleNext(const std::vector<EntityId>& stack,
                                  std::optional<EntityId> current);

// Lasso (freeform) selection along the closed `polygon`: window == entities lying completely
// inside it, crossing == entities that touch or lie inside it. Ascending id order.
std::vector<EntityId> selectInPolygon(const Document& doc, const index::SpatialIndex& index,
                                      const std::vector<Vec2>& polygon, bool crossing);

// Fence selection: entities whose outline touches the open polyline `fence`. Ascending id order.
std::vector<EntityId> selectAlongFence(const Document& doc, const index::SpatialIndex& index,
                                       const std::vector<Vec2>& fence);

// Window selection (crossing == false): entities lying completely inside the box. Crossing
// selection: entities that touch or lie inside it. Ascending id order.
std::vector<EntityId> selectInBox(const Document& doc, const index::SpatialIndex& index,
                                  const BBox2& box, bool crossing);

// Rectangle between two screen-space corners converted by the caller. Dragging left to right is
// a window, right to left a crossing.
inline bool dragIsCrossing(double startX, double endX) {
    return endX < startX;
}

}  // namespace tbcad::ui
