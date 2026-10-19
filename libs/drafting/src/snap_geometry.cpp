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

// Segments of a path of vertices as lines, and the vertices as end points.
void vertexPath(const std::vector<Vec2>& v, bool closed, SnapGeometry& out) {
    for (const Vec2& p : v)
        out.endpoints.push_back(p);
    const std::size_t segs = v.size() < 2 ? 0 : (closed ? v.size() : v.size() - 1);
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec2 a = v[i], b = v[(i + 1) % v.size()];
        if (distance(a, b) <= 0)
            continue;
        out.curves.push_back(geom::CurveRef::line(a, b));
        out.midpoints.push_back((a + b) * 0.5);
    }
}

void textProvider(const Document&, const Entity& e, SnapGeometry& out) {
    if (const auto* t = entityAs<TextData>(e)) {
        // Aligned text is anchored at its alignment point; left-baseline text at its position.
        const bool anchored = t->h != HAlign::Left || t->v != VAlign::Baseline;
        out.insertions.push_back(anchored ? t->alignPoint : t->position);
    }
}

void mtextProvider(const Document&, const Entity& e, SnapGeometry& out) {
    if (const auto* t = entityAs<MTextData>(e))
        out.insertions.push_back(t->position);
}

void dimensionProvider(const Document&, const Entity& e, SnapGeometry& out) {
    if (const auto* d = entityAs<DimensionData>(e)) {
        for (const Vec2& p : d->defPts)
            out.nodes.push_back(p);
        out.insertions.push_back(d->textPos);
    }
}

void leaderProvider(const Document&, const Entity& e, SnapGeometry& out) {
    if (const auto* l = entityAs<LeaderData>(e))
        vertexPath(l->vertices, false, out);
}

void mleaderProvider(const Document&, const Entity& e, SnapGeometry& out) {
    if (const auto* m = entityAs<MLeaderData>(e)) {
        for (const auto& line : m->lines)
            vertexPath(line, false, out);
        out.endpoints.push_back(m->landing);
        if (m->dogleg && distance(m->landing, m->contentPos) > 0)
            out.curves.push_back(geom::CurveRef::line(m->landing, m->contentPos));
        out.insertions.push_back(m->contentPos);
    }
}

void rayProvider(const Document&, const Entity& e, SnapGeometry& out) {
    if (const auto* r = entityAs<RayData>(e)) {
        if (!r->infinite)
            out.endpoints.push_back(r->origin);  // an xline has no end
        geom::curvesOf(e, out.curves);
    }
}

void mlineProvider(const Document&, const Entity& e, SnapGeometry& out) {
    if (const auto* m = entityAs<MlineData>(e))
        vertexPath(m->v, m->closed, out);
}

// The boundary of a hatch snaps like the objects it was made from: end points, mid points,
// centres and quadrants of its edges, and its edges as curves.
void hatchProvider(const Document& doc, const Entity& e, SnapGeometry& out) {
    const auto* h = entityAs<HatchData>(e);
    if (!h)
        return;
    auto edge = [&](Entity&& part) { curveProvider(doc, part, out); };
    for (const HatchLoop& loop : h->loops) {
        for (const HatchEdge& ed : loop.edges) {
            if (const auto* l = std::get_if<HatchLineEdge>(&ed)) {
                if (distance(l->a, l->b) > 0)
                    edge(Entity{{}, LineData{l->a, l->b}});
            } else if (const auto* a = std::get_if<HatchArcEdge>(&ed)) {
                // A clockwise edge is the counter-clockwise arc from its end to its start.
                edge(Entity{{}, a->ccw ? ArcData{a->center, a->radius, a->startAngle, a->endAngle}
                                       : ArcData{a->center, a->radius, a->endAngle, a->startAngle}});
            } else if (const auto* el = std::get_if<HatchEllipseEdge>(&ed)) {
                edge(Entity{{}, el->ccw ? EllipseData{el->center, el->majorAxis, el->ratio,
                                                      el->startParam, el->endParam}
                                        : EllipseData{el->center, el->majorAxis, el->ratio,
                                                      el->endParam, el->startParam}});
            } else if (const auto* sp = std::get_if<HatchSplineEdge>(&ed)) {
                edge(Entity{{}, sp->spline});
            } else if (const auto* pl = std::get_if<HatchPolylineEdge>(&ed)) {
                PolylineData pd;
                pd.v = pl->v;
                pd.closed = pl->closed;
                edge(Entity{{}, std::move(pd)});
            }
        }
    }
}

}  // namespace

SnapGeometryRegistry::SnapGeometryRegistry() {
    for (EntityKind k : {EntityKind::Line, EntityKind::Circle, EntityKind::Arc,
                         EntityKind::Polyline, EntityKind::Ellipse, EntityKind::Spline})
        set(k, curveProvider);
    set(EntityKind::Point, pointProvider);
    set(EntityKind::Text, textProvider);
    set(EntityKind::MText, mtextProvider);
    set(EntityKind::Dimension, dimensionProvider);
    set(EntityKind::Leader, leaderProvider);
    set(EntityKind::MLeader, mleaderProvider);
    set(EntityKind::Ray, rayProvider);
    set(EntityKind::MLine, mlineProvider);
    set(EntityKind::Hatch, hatchProvider);
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

bool SnapGeometryRegistry::collect(const Document& doc, const Entity& e, SnapGeometry& out) const {
    const SnapGeometryProvider* p = find(kind(e));
    if (!p)
        return false;
    (*p)(doc, e, out);
    return true;
}

}  // namespace tbcad::drafting
