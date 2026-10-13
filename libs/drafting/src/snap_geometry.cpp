#include "tbcad/drafting/snap_geometry.h"

#include "tbcad/geom/entity.h"

namespace tbcad::drafting {

namespace {

void appendAll(std::vector<Vec2>& dst, std::vector<Vec2>&& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

void curveProvider(const Document&, const Entity& e, SnapGeometry& out) {
    appendAll(out.endpoints, geom::endpoints(e));
    appendAll(out.midpoints, geom::midpoints(e));
    appendAll(out.centers, geom::centers(e));
    appendAll(out.quadrants, geom::quadrants(e));
    geom::curvesOf(e, out.curves);
}

void pointProvider(const Document&, const Entity& e, SnapGeometry& out) {
    if (const auto* p = entityAs<PointData>(e))
        out.nodes.push_back(p->p);
}

}  // namespace

SnapGeometryRegistry::SnapGeometryRegistry() {
    for (EntityKind k : {EntityKind::Line, EntityKind::Circle, EntityKind::Arc,
                         EntityKind::Polyline, EntityKind::Ellipse, EntityKind::Spline})
        set(k, curveProvider);
    set(EntityKind::Point, pointProvider);
}

SnapGeometryRegistry& SnapGeometryRegistry::instance() {
    static SnapGeometryRegistry registry;
    return registry;
}

void SnapGeometryRegistry::set(EntityKind kind, SnapGeometryProvider provider) {
    providers_[static_cast<std::size_t>(kind)] = std::move(provider);
}

void SnapGeometryRegistry::clear(EntityKind kind) {
    providers_[static_cast<std::size_t>(kind)] = nullptr;
}

const SnapGeometryProvider* SnapGeometryRegistry::find(EntityKind k) const {
    const auto& p = providers_[static_cast<std::size_t>(k)];
    return p ? &p : nullptr;
}

bool SnapGeometryRegistry::collect(const Document& doc, const Entity& e,
                                   SnapGeometry& out) const {
    const SnapGeometryProvider* p = find(kind(e));
    if (!p)
        return false;
    (*p)(doc, e, out);
    return true;
}

}  // namespace tbcad::drafting
