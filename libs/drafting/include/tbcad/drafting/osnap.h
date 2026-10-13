#pragma once

#include <optional>
#include <vector>

#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/snap_geometry.h"
#include "tbcad/index/spatial_index.h"

namespace tbcad::drafting {

struct SnapRequest {
    Vec2 cursor{};
    std::optional<Vec2> base{};  // needed for perpendicular and tangent
    double apertureMm = 1;       // 10 px at the current zoom
    SnapModeMask modes = kDefaultSnapModes;
    // At most this many entities (the ones nearest to the cursor) are examined; intersections
    // are only computed among them.
    std::size_t maxEntities = 200;
};

struct SnapCandidate {
    Vec2 point{};
    SnapMode mode = SnapMode::Endpoint;
    double distance = 0;  // from the cursor
    EntityId entity{};
    EntityId entity2{};  // second entity of an intersection
};

// All snap points within the aperture, best first: smaller distance wins; equal distances (within
// 1e-6 mm) go to the mode that comes first in SnapMode. Entities on layers that are off or
// frozen and entities marked invisible never snap. The best candidate is front().
std::vector<SnapCandidate> findSnapCandidates(const Document& doc, const index::SpatialIndex& index,
                                              const SnapRequest& request,
                                              const SnapGeometryRegistry& providers =
                                                  SnapGeometryRegistry::instance());

// Snap candidates of a set of curves, for tests and for modules that snap to their own geometry.
void curveSnapPoints(const geom::CurveRef& curve, const SnapRequest& request,
                     std::vector<SnapCandidate>& out, EntityId id = {});

}  // namespace tbcad::drafting
