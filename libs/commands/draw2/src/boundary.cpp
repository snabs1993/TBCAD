#include <algorithm>
#include <cmath>

#include "commands.h"
#include "common.h"
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

// Visible model-space entities that can bound a region; when the host has a viewport and an
// index only those inside the view are offered.
std::vector<hatch::Candidate> candidates(cmd::CommandContext& ctx) {
    const Document& doc = ctx.doc();
    const cmd::ViewState view = ctx.host().view();
    const bool all = !(view.sizePx.x > 0 && view.sizePx.y > 0 && view.scale > 0);
    BBox2 window;
    if (!all) {
        const Vec2 half{view.sizePx.x * view.scale / 2, view.sizePx.y * view.scale / 2};
        window.expand(view.center - half);
        window.expand(view.center + half);
    }
    std::vector<hatch::Candidate> out;
    auto consider = [&](const Entity& e) {
        if (!e.common.visible || !hasCurves(e) || doc.ownerBlock(e.common.id))
            return;
        if (!doc.effective(e.common).live())
            return;
        out.push_back({e.common.id, &e});
    };
    if (!all) {
        if (auto ids = ctx.host().entitiesInWindow(window)) {
            for (EntityId id : *ids)
                if (const Entity* e = doc.find(id))
                    consider(*e);
            return out;
        }
    }
    doc.forEachEntity([&](const Entity& e) {
        if (all || window.intersects(geom::bounds(e)))
            consider(e);
    });
    return out;
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

// BOUNDARY [BO]: a closed polyline for the region around each picked point, and one for every
// island inside it. Uses the same boundary finder as HATCH.
class BoundaryCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BOUNDARY"; }
    std::vector<std::string> defaultAliases() const override { return {"BO"}; }
    std::string_view description() const override {
        return "Create a closed polyline from the region around a point";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        for (;;) {
            auto p = co_await ctx.getPoint("Specify internal point");
            if (!p.ok())
                co_return;
            auto found = hatch::findBoundary(candidates(ctx), p.point);
            if (!found) {
                ctx.message(found.error().message);
                continue;
            }
            ctx.checkpoint();
            for (const HatchLoop& loop : found.value().loops)
                addEntity(ctx, loopPolyline(loop));
            ctx.message(
                std::to_string(found.value().loops.size()) +
                (found.value().loops.size() == 1 ? " polyline created." : " polylines created."));
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBoundary() {
    return std::make_unique<BoundaryCommand>();
}

}  // namespace tbcad::draw2::detail
