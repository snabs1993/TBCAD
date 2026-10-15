#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "dim_internal.h"
#include "tbcad/dim/dim.h"
#include "tbcad/geom/geom.h"
#include "tbcad/text/text.h"

namespace tbcad::dimcmd {

void previewEntity(cmd::CommandContext& ctx, Entity e, DisplayList& out) {
    applyCurrentProperties(ctx.doc().header(), e);
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
    applyCurrentProperties(doc.header(), e);
    return e;
}

void previewDim(cmd::CommandContext& ctx, const DimensionData& d, DisplayList& out) {
    previewEntity(ctx, dimEntity(ctx.doc(), d), out);
}

namespace {

// The first dimension in a drawing drawn at a large scale (a plan in millimetres, seen from far
// away) would carry 2.5 mm text, too small to see at that zoom. Give it a style scaled to the
// current view so its text is readable (about 12 screen pixels), as 1-2-5 steps like 1:50, 1:100.
// Later dimensions use the same style, and the style can still be changed in Properties.
void scaleFirstDimensionToView(cmd::CommandContext& ctx) {
    const Document& doc = ctx.doc();
    const DimStyle& cur = dim::styleOf(doc, doc.header().currentDimStyle);
    if (cur.id != kDimStyleStandard || cur.overallScale != 1)
        return;
    // How big the drawing is decides too (a script has no view; a plan printed on a sheet needs
    // text about extent/160 tall): the larger of the two wishes wins.
    bool any = false;
    BBox2 box;
    doc.forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Dimension)
            any = true;
        else if (!doc.ownerBlock(e.common.id))
            box.merge(geom::bounds(e, doc));
    });
    if (any)
        return;
    double wanted = 0;
    const double pixelMm = ctx.host().view().scale;
    if (pixelMm > 0 && std::isfinite(pixelMm))
        wanted = 12.0 * pixelMm / std::max(cur.textHeight, 1e-6);
    if (!box.empty()) {
        const double extent = std::max(box.max.x - box.min.x, box.max.y - box.min.y);
        if (std::isfinite(extent))
            wanted = std::max(wanted, extent / 160.0 / std::max(cur.textHeight, 1e-6));
    }
    if (wanted < 8)
        return;  // the normal case: the text is readable as it is
    double n = 1;
    for (double decade = 1; decade <= 1e7; decade *= 10)
        for (double step : {1.0, 2.0, 5.0})
            if (std::fabs(std::log(decade * step / wanted)) < std::fabs(std::log(n / wanted)))
                n = decade * step;
    char tag[40];
    std::snprintf(tag, sizeof tag, "-1to%g", n);
    DimStyle copy = cur;
    copy.id = {};
    copy.name = cur.name + tag;
    copy.overallScale = n;
    if (const DimStyle* have = doc.dimStyleByName(copy.name)) {
        (void)ctx.tx().modifyHeader([id = have->id](Header& h) { h.currentDimStyle = id; });
        return;
    }
    if (auto id = ctx.tx().addDimStyle(std::move(copy)))
        (void)ctx.tx().modifyHeader([i = id.value()](Header& h) { h.currentDimStyle = i; });
}

}  // namespace

bool addDim(cmd::CommandContext& ctx, DimensionData d) {
    ctx.setUndoLabel(std::string(ctx.commandName()));
    scaleFirstDimensionToView(ctx);
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
    return leaderOf(doc, std::move(pts), plain, doc.header().currentDimStyle);
}

LeaderData leaderOf(const Document& doc, std::vector<Vec2> pts, const std::string& plain,
                    DimStyleId style) {
    LeaderData l;
    l.style = style;
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
