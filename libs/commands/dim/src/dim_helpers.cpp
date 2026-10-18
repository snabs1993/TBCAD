#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

#include "dim_internal.h"
#include "tbcad/core/dim_assoc.h"
#include "tbcad/dim/dim.h"
#include "tbcad/geom/geom.h"
#include "tbcad/text/annotation_size.h"
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

// The first dimension or leader in a drawing drawn at a large scale (a plan in millimetres, seen
// from far away) would carry 2.5 mm text, too small to see at that zoom. Give it a style scaled
// like new text is (the annotation scale, else the view and the size of the drawing), as 1-2-5
// steps like 1:50, 1:100. Later ones use the same style, and the style can still be changed in
// Properties.
void scaleFirstDimensionToView(cmd::CommandContext& ctx) {
    const Document& doc = ctx.doc();
    const DimStyle& cur = dim::styleOf(doc, doc.header().currentDimStyle);
    if (cur.id != kDimStyleStandard || cur.overallScale != 1)
        return;
    // Annotative dimensions already follow the annotation scale; scaling the style too would
    // multiply the two.
    if (annotateNewObjects(doc.header()))
        return;
    bool any = false;
    doc.forEachEntity([&](const Entity& e) {
        const EntityKind k = kind(e);
        any = any || k == EntityKind::Dimension || k == EntityKind::Leader;
    });
    if (any)
        return;
    const double n = text::suggestedAnnotationScale(
        doc, ctx.host().view().scale, static_cast<bool>(ctx.host().drawingOwner()), cur.textHeight);
    if (n <= 1)
        return;  // the normal case: the text is readable as it is
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

void scaleFirstAnnotationToView(cmd::CommandContext& ctx) {
    scaleFirstDimensionToView(ctx);
}

bool addDim(cmd::CommandContext& ctx, DimensionData d) {
    ctx.setUndoLabel(std::string(ctx.commandName()));
    scaleFirstDimensionToView(ctx);
    {
        // An annotative dimension on a style that is already enlarged comes out too big: say so
        // once per drawing.
        static std::set<std::uint64_t> warned;
        const Document& doc = ctx.doc();
        const DimStyle& cur = dim::styleOf(doc, doc.header().currentDimStyle);
        const double annoScale = currentAnnotationScale(doc.header());
        if (annotateNewObjects(doc.header()) && annoScale > 1 && cur.overallScale > 1 &&
            warned.insert(doc.uid()).second)
            ctx.message("Warning: dimension style \"" + cur.name +
                        "\" already has an overall scale of " +
                        std::to_string(static_cast<int>(cur.overallScale)) +
                        " and new dimensions are annotative at 1:" +
                        std::to_string(static_cast<int>(annoScale)) +
                        ", so sizes multiply. Set the style's overall scale to 1.");
    }
    linkDimension(ctx.doc(), d, ctx.tx().defaultOwner());
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

bool straightPieceAt(const Entity& e, Vec2 pick, Vec2& a, Vec2& b) {
    if (const auto* l = entityAs<LineData>(e)) {
        a = l->a;
        b = l->b;
        return true;
    }
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl)
        return false;
    // The straight segment nearest to the pick.
    const std::size_t n = pl->v.size();
    const std::size_t segs = pl->closed ? n : (n ? n - 1 : 0);
    double best = std::numeric_limits<double>::infinity();
    bool found = false;
    for (std::size_t i = 0; i < segs; ++i) {
        const Vec2 p = pl->v[i].p, q = pl->v[(i + 1) % n].p;
        if (pl->v[i].bulge != 0 || distance(p, q) < tol::kLinear)
            continue;
        const Vec2 d = q - p;
        const double t = std::clamp(dot(pick - p, d) / lengthSquared(d), 0.0, 1.0);
        const double dist = distance(pick, p + d * t);
        if (dist < best) {
            best = dist;
            a = p;
            b = q;
            found = true;
        }
    }
    return found;
}

bool isCircleOrArc(const Entity& e) {
    if (const auto* pl = entityAs<PolylineData>(e)) {
        for (const PolyVertex& v : pl->v)
            if (v.bulge != 0)
                return true;
        return false;
    }
    return kind(e) == EntityKind::Circle || kind(e) == EntityKind::Arc;
}

// Dimensionable straight piece of an object: a line, the ends of an arc, the segment of a
// polyline nearest to the pick.
bool isMeasurable(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
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
    // An ellipse or spline is measured end to end (a full ellipse across its major axis).
    if (const auto* el = entityAs<EllipseData>(e)) {
        if (std::abs((el->endParam - el->startParam) - kTwoPi) < 1e-9) {
            a = el->center - el->majorAxis;
            b = el->center + el->majorAxis;
            return true;
        }
    }
    if (kind(e) == EntityKind::Ellipse || kind(e) == EntityKind::Spline) {
        const std::vector<Vec2> ends = geom::endpoints(e);
        if (ends.size() < 2)
            return false;
        a = ends.front();
        b = ends.back();
        return true;
    }
    return false;
}

bool circleOf(const Entity& e, Vec2& c, double& r, const Vec2* pick) {
    if (const auto* pl = entityAs<PolylineData>(e)) {
        // The bulged segment nearest to the pick (the first one without a pick).
        const std::size_t n = pl->v.size();
        const std::size_t segs = pl->closed ? n : (n ? n - 1 : 0);
        double best = std::numeric_limits<double>::infinity();
        bool found = false;
        for (std::size_t i = 0; i < segs; ++i) {
            const PolyVertex& v = pl->v[i];
            const auto arc = geom::bulgeArc(v.p, pl->v[(i + 1) % n].p, v.bulge);
            if (!arc || !(arc->radius > 0))
                continue;
            double dist = 0;
            if (pick) {
                // Distance to the arc itself, not to its circle.
                const double a = std::atan2(pick->y - arc->center.y, pick->x - arc->center.x);
                double rel = std::fmod(a - arc->startAngle, kTwoPi);
                if (rel < 0)
                    rel += kTwoPi;
                dist = rel <= arc->sweep
                           ? std::abs(distance(*pick, arc->center) - arc->radius)
                           : std::min(distance(*pick, v.p), distance(*pick, pl->v[(i + 1) % n].p));
            }
            if (!found || dist < best) {
                best = dist;
                c = arc->center;
                r = arc->radius;
                found = true;
            }
        }
        return found;
    }
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
// The axes are the UCS axes (`ucs` is the angle of its X axis).
double autoRotation(Vec2 p1, Vec2 p2, Vec2 c, double ucs) {
    const double co = std::cos(-ucs), si = std::sin(-ucs);
    auto in = [&](Vec2 p) { return Vec2{p.x * co - p.y * si, p.x * si + p.y * co}; };
    p1 = in(p1);
    p2 = in(p2);
    c = in(c);
    BBox2 b;
    b.expand(p1);
    b.expand(p2);
    double dx = std::max({c.x - b.max.x, b.min.x - c.x, 0.0});
    double dy = std::max({c.y - b.max.y, b.min.y - c.y, 0.0});
    return ucs + (dx > dy ? kPi / 2 : 0.0);
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
