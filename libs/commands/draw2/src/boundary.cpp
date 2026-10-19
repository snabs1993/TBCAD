#include <algorithm>
#include <cmath>
#include <deque>

#include "commands.h"
#include "common.h"
#include "tbcad/blocks/blocks.h"
#include "tbcad/hatch/hatch.h"

namespace tbcad::draw2::detail {

namespace {

bool hasCurves(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Polyline:
        case EntityKind::Ellipse:
        case EntityKind::Spline:
            return true;
        default:
            return false;
    }
}

// Visible model-space entities that can bound a region and meet `window`. A block reference
// offers the visible curves it is made of, copied into `store` (cleared first) in world space.
std::vector<hatch::Candidate> candidatesIn(cmd::CommandContext& ctx, const BBox2& window,
                                           std::deque<Entity>& store) {
    const Document& doc = ctx.doc();
    store.clear();
    std::vector<hatch::Candidate> out;
    auto consider = [&](const Entity& e) {
        const bool insert = kind(e) == EntityKind::Insert;
        if (!e.common.visible || !(insert || hasCurves(e)) || doc.ownerBlock(e.common.id))
            return;
        if (!doc.effective(e.common).live())
            return;
        if (!insert) {
            out.push_back({e.common.id, &e});
            return;
        }
        for (Entity& part : blocks::snapGeometry(doc, e)) {
            if (!hasCurves(part) || !doc.effective(part.common).live())
                continue;
            store.push_back(std::move(part));
            out.push_back({e.common.id, &store.back()});
        }
    };
    if (auto ids = ctx.host().entitiesInWindow(window)) {
        for (EntityId id : *ids)
            if (const Entity* e = doc.find(id))
                consider(*e);
        return out;
    }
    doc.forEachEntity([&](const Entity& e) {
        const BBox2 box = kind(e) == EntityKind::Insert ? blocks::insertBounds(doc, e)
                                                        : geom::bounds(e);
        if (window.intersects(box))
            consider(e);
    });
    return out;
}

// The view, or the drawing's extents when the host has no viewport.
BBox2 searchWindow(cmd::CommandContext& ctx) {
    const cmd::ViewState view = ctx.host().view();
    BBox2 window;
    if (view.sizePx.x > 0 && view.sizePx.y > 0 && view.scale > 0) {
        const Vec2 half{view.sizePx.x * view.scale / 2, view.sizePx.y * view.scale / 2};
        window.expand(view.center - half);
        window.expand(view.center + half);
    } else {
        ctx.doc().forEachEntity([&](const Entity& e) { window.merge(geom::bounds(e)); });
    }
    return window;
}

void pushPoint(std::vector<PolyVertex>& v, Vec2 p, double bulge = 0) {
    if (!v.empty() && tol::isZeroLength(v.back().p - p)) {
        if (bulge != 0)
            v.back().bulge = bulge;
        return;
    }
    PolyVertex pv;
    pv.p = p;
    pv.bulge = bulge;
    v.push_back(pv);
}

// A hatch loop as a closed polyline: lines stay lines, circular arcs become bulges, everything
// else is flattened.
PolylineData loopPolyline(const HatchLoop& loop) {
    PolylineData d;
    d.closed = true;
    auto curves = hatch::loopCurves(loop);
    if (curves.empty())
        return d;
    if (curves.size() == 1 && curves[0].isClosed() &&
        curves[0].kind() == geom::CurveRef::Kind::Arc) {
        const auto& c = curves[0];
        pushPoint(d.v, c.pointAt(c.t0()), 1);
        pushPoint(d.v, c.pointAt(c.t0() + kPi), 1);
        return d;
    }
    Vec2 cur{};
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const geom::CurveRef& c = curves[i];
        const Vec2 a = c.startPoint(), b = c.endPoint();
        bool forward;
        if (i == 0 && curves.size() > 1) {
            const Vec2 na = curves[1].startPoint(), nb = curves[1].endPoint();
            const double dEnd = std::min(distance(b, na), distance(b, nb));
            const double dStart = std::min(distance(a, na), distance(a, nb));
            forward = dEnd <= dStart;
        } else {
            forward = distance(a, cur) <= distance(b, cur);
        }
        const Vec2 from = forward ? a : b, to = forward ? b : a;
        switch (c.kind()) {
            case geom::CurveRef::Kind::Line:
                pushPoint(d.v, from);
                break;
            case geom::CurveRef::Kind::Arc: {
                const Vec2 mid = c.pointAt(c.t0() + c.sweep() / 2);
                pushPoint(d.v, from, geom::bulgeThrough(from, mid, to).value_or(0));
                break;
            }
            default: {
                std::vector<Vec2> pts;
                geom::flatten(c, 0.01, pts);
                if (!forward)
                    std::reverse(pts.begin(), pts.end());
                if (!pts.empty())
                    pts.pop_back();
                for (const Vec2& p : pts)
                    pushPoint(d.v, p);
                break;
            }
        }
        cur = to;
    }
    if (d.v.size() > 1 && tol::isZeroLength(d.v.front().p - d.v.back().p)) {
        d.v.pop_back();
    }
    return d;
}

constexpr const char* kIslands = "boundary.islands";  // 0 Normal, 1 Outer, 2 Ignore

hatch::IslandStyle islandStyleOf(double v) {
    return v >= 1.5   ? hatch::IslandStyle::Ignore
           : v >= 0.5 ? hatch::IslandStyle::Outer
                      : hatch::IslandStyle::Normal;
}

// BOUNDARY [BO]: a closed polyline for the region around each picked point, and one for every
// island inside it. Uses the same boundary finder and gap tolerance (hatch.gap) as HATCH. The
// Islands keyword picks how islands are treated: Normal (every closed shape inside), Outer (only
// the first level) or Ignore (the outline alone).
class BoundaryCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BOUNDARY"; }
    std::vector<std::string> defaultAliases() const override { return {"BO", "BPOLY", "-BOUNDARY"}; }
    std::string_view description() const override {
        return "Create a closed polyline from the region around a point";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        for (;;) {
            cmd::PointOptions po;
            po.keywords = {"Islands"};
            auto p = co_await ctx.getPoint("Specify internal point or [Islands]", std::move(po));
            if (p.status == cmd::Status::Keyword) {
                cmd::KeywordOptions ko;
                ko.keywords = {"Normal", "Outer", "Ignore"};
                const hatch::IslandStyle cur = islandStyleOf(settingNumber(ctx, kIslands, 0));
                ko.defaultKeyword = cur == hatch::IslandStyle::Ignore  ? "Ignore"
                                    : cur == hatch::IslandStyle::Outer ? "Outer"
                                                                       : "Normal";
                auto st = co_await ctx.getKeyword("Island detection [Normal/Outer/Ignore]",
                                                  std::move(ko));
                if (!st.ok())
                    co_return;
                saveSetting(ctx, kIslands,
                            st.keyword == "Ignore"  ? 2
                            : st.keyword == "Outer" ? 1
                                                    : 0);
                continue;
            }
            if (!p.ok())
                co_return;
            // The gap tolerance is HATCH's (hatch.gap, HPGAPTOL): ends that stop short of each
            // other by less than that are joined.
            hatch::BoundaryOptions bo;
            bo.gapTolerance = std::max(0.0, settingNumber(ctx, "hatch.gap", 0));
            std::deque<Entity> store;
            auto found = hatch::findBoundaryNear(
                             [&](const BBox2& w) { return candidatesIn(ctx, w, store); },
                             searchWindow(ctx), p.point, bo)
                             .result;
            if (!found) {
                ctx.message(found.error().message);
                continue;
            }
            hatch::Boundary boundary = std::move(found.value());
            hatch::applyIslandStyle(boundary, islandStyleOf(settingNumber(ctx, kIslands, 0)),
                                    [&](EntityId id) { return ctx.doc().find(id); });
            ctx.checkpoint();
            for (const HatchLoop& loop : boundary.loops)
                addEntity(ctx, loopPolyline(loop));
            ctx.message(std::to_string(boundary.loops.size()) + (boundary.loops.size() == 1
                                                                     ? " polyline created."
                                                                     : " polylines created."));
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBoundary() {
    return std::make_unique<BoundaryCommand>();
}

}  // namespace tbcad::draw2::detail
