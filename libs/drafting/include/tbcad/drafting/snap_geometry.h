#pragma once

#include <array>
#include <functional>
#include <vector>

#include "tbcad/core/core.h"
#include "tbcad/geom/curve.h"

namespace tbcad::drafting {

// What one entity offers to object snap: ready-made points, and the curves that nearest,
// perpendicular, tangent and intersection are computed from.
struct SnapGeometry {
    std::vector<Vec2> endpoints{};
    std::vector<Vec2> midpoints{};
    std::vector<Vec2> centers{};
    std::vector<Vec2> quadrants{};
    std::vector<Vec2> geometricCenters{};  // centroids (GCE)
    std::vector<Vec2> nodes{};
    std::vector<Vec2> insertions{};  // insertion points of text, mtext and blocks
    std::vector<geom::CurveRef> curves{};
};

// Fills `out` with the snap geometry of `e` in world coordinates. W5-3 registers one for Insert
// that walks the block definition, applies the insert transform and appends the sub-entities'
// points and curves.
using SnapGeometryProvider = std::function<void(const Document&, const Entity&, SnapGeometry&)>;

// Providers keyed by entity kind. A fresh registry holds the built-in providers for Line,
// Circle, Arc, Polyline, Ellipse, Spline (curves plus the geom snap helpers), Point (node), Text,
// MText (insertion point), Dimension (definition points as nodes, text as insertion), Leader,
// MLeader (vertices, lines), Ray (origin, line) and Mline (vertices).
class SnapGeometryRegistry {
public:
    SnapGeometryRegistry();

    // The registry the application's object snap uses. Modules add providers to it at start-up.
    static SnapGeometryRegistry& instance();

    // Replaces any provider already set for the kind.
    void set(EntityKind kind, SnapGeometryProvider provider);
    void clear(EntityKind kind);
    const SnapGeometryProvider* find(EntityKind kind) const;

    // False for kinds without a provider (text, hatch, ...): they never produce snap points.
    bool collect(const Document& doc, const Entity& e, SnapGeometry& out) const;

private:
    std::array<SnapGeometryProvider, kEntityKindCount> providers_{};
};

}  // namespace tbcad::drafting
