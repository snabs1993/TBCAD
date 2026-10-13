#include <algorithm>
#include <cmath>
#include <limits>

#include "dim_internal.h"
#include "tbcad/dim/dim.h"
#include "tbcad/geom/geom.h"
#include "tbcad/text/text.h"

namespace tbcad::dimcmd {

void previewEntity(cmd::CommandContext& ctx, Entity e, DisplayList& out) {
    e.common.layer = ctx.doc().header().currentLayer;
    geom::DisplayContext dc;
    const double scale = ctx.host().view().scale;
    if (scale > 0)
        dc.pixelSizeMm = scale;
    try {
        geom::buildDisplay(ctx.doc(), e, dc, out);
    } catch (...) {
        // a half-defined preview simply shows nothing
    }
}

Entity dimEntity(const Document& doc, DimensionData d) {
    d.style = doc.header().currentDimStyle;
    dim::refresh(doc, d);
    Entity e;
    e.data = Indirect<DimensionData>(std::move(d));
    return e;
}

void previewDim(cmd::CommandContext& ctx, const DimensionData& d, DisplayList& out) {
    previewEntity(ctx, dimEntity(ctx.doc(), d), out);
}

bool addDim(cmd::CommandContext& ctx, DimensionData d) {
    ctx.setUndoLabel(std::string(ctx.commandName()));
    auto id = ctx.tx().add(dimEntity(ctx.doc(), std::move(d)));
    if (!id) {
        ctx.message(id.error().message);
        return false;
    }
    return true;
}

void previewLine(cmd::CommandContext& ctx, Vec2 a, Vec2 b, DisplayList& out) {
    Entity e;
    e.data = LineData{a, b};
    previewEntity(ctx, std::move(e), out);
}

bool isLine(const Entity& e) {
    return kind(e) == EntityKind::Line;
}

bool isCircleOrArc(const Entity& e) {
    return kind(e) == EntityKind::Circle || kind(e) == EntityKind::Arc;
}

// Dimensionable straight piece of an object: a line, the ends of an arc, the segment of a
// polyline nearest to the pick.
bool isMeasurable(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Arc:
        case EntityKind::Polyline:
            return true;
        default:
            return false;
    }
}

bool measurableEnds(const Entity& e, Vec2 pick, Vec2& a, Vec2& b) {
    if (const auto* l = entityAs<LineData>(e)) {
        a = l->a;
        b = l->b;
        return true;
    }
    if (const auto* arc = entityAs<ArcData>(e)) {
        a = arc->center + Vec2{std::cos(arc->startAngle), std::sin(arc->startAngle)} * arc->radius;
        b = arc->center + Vec2{std::cos(arc->endAngle), std::sin(arc->endAngle)} * arc->radius;
        return true;
    }
    if (const auto* pl = entityAs<PolylineData>(e)) {
        const std::size_t n = pl->v.size();
        if (n < 2)
            return false;
        const std::size_t segs = pl->closed ? n : n - 1;
        double best = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < segs; ++i) {
            Vec2 p = pl->v[i].p, q = pl->v[(i + 1) % n].p;
            Vec2 d = q - p;
            double l2 = lengthSquared(d);
            double t = l2 > 0 ? std::clamp(dot(pick - p, d) / l2, 0.0, 1.0) : 0;
            double dist = distance(pick, p + d * t);
            if (dist < best) {
                best = dist;
                a = p;
                b = q;
            }
        }
        return true;
    }
    return false;
}

bool circleOf(const Entity& e, Vec2& c, double& r) {
    if (const auto* ci = entityAs<CircleData>(e)) {
        c = ci->center;
        r = ci->radius;
        return r > 0;
    }
    if (const auto* ar = entityAs<ArcData>(e)) {
        c = ar->center;
        r = ar->radius;
        return r > 0;
    }
    return false;
}

// Horizontal or vertical dimension line, whichever side of the points the cursor is on.
double autoRotation(Vec2 p1, Vec2 p2, Vec2 c) {
    BBox2 b;
    b.expand(p1);
    b.expand(p2);
    double dx = std::max({c.x - b.max.x, b.min.x - c.x, 0.0});
    double dy = std::max({c.y - b.max.y, b.min.y - c.y, 0.0});
    return dx > dy ? kPi / 2 : 0.0;
}

LeaderData leaderOf(const Document& doc, std::vector<Vec2> pts, const std::string& plain) {
    LeaderData l;
    l.style = doc.header().currentDimStyle;
    l.vertices = std::move(pts);
    l.utf8 = text::markupFromPlain(plain);
    if (l.vertices.size() >= 2) {
        const DimStyle& st = dim::styleOf(doc, l.style);
        const double k = st.overallScale > 0 ? st.overallScale : 1.0;
        const Vec2 last = l.vertices.back(), prev = l.vertices[l.vertices.size() - 2];
        const double sign = last.x >= prev.x ? 1 : -1;
        l.textPos = last + Vec2{sign * (st.arrowSize + st.textGap) * k, 0};
    }
    return l;
}

}  // namespace tbcad::dimcmd
