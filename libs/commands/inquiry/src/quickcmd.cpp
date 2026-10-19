#include <algorithm>
#include <cmath>

#include "extra.h"
#include "tbcad/geom/geom.h"

namespace tbcad::inquiry {

namespace {

constexpr double kAperturePx = 24;

std::vector<const Entity*> entitiesNear(cmd::CommandContext& ctx, Vec2 at) {
    const Document& doc = ctx.doc();
    const double scale = ctx.host().view().scale > 0 ? ctx.host().view().scale : 1;
    const double r = kAperturePx * scale;
    BBox2 window;
    window.expand({at.x - r, at.y - r});
    window.expand({at.x + r, at.y + r});
    std::vector<const Entity*> out;
    auto consider = [&](const Entity& e) {
        if (!e.common.visible || doc.ownerBlock(e.common.id) || !doc.effective(e.common).live())
            return;
        std::vector<geom::CurveRef> curves;
        if (!geom::curvesOf(e, curves))
            return;
        for (const auto& c : curves)
            if (c.distanceTo(at) <= r) {
                out.push_back(&e);
                return;
            }
    };
    if (auto ids = ctx.host().entitiesInWindow(window)) {
        for (EntityId id : *ids)
            if (const Entity* e = doc.find(id))
                consider(*e);
    } else {
        doc.forEachEntity([&](const Entity& e) {
            if (window.intersects(geom::bounds(e)))
                consider(e);
        });
    }
    return out;
}

Precision precisionOf(cmd::CommandContext& ctx) {
    if (auto s = ctx.host().setting("inquiry.precision")) {
        try {
            return std::clamp(std::stoi(*s), 0, 12);
        } catch (...) {
        }
    }
    return linearPrecision(ctx.doc().header());  // UNITS precision
}

// QUICKMEASURE [QM]: hover over the drawing to see the length, radius and gap of the objects under
// the cursor; a click writes the numbers to the command line. Esc finishes.
class QuickMeasure final : public cmd::Command {
public:
    std::string_view name() const override { return "QUICKMEASURE"; }
    std::vector<std::string> defaultAliases() const override { return {"QM"}; }
    std::string_view description() const override {
        return "Show distances between nearby objects under the cursor";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        const Precision prec = precisionOf(ctx);
        for (;;) {
            cmd::PointOptions o;
            o.allowNone = false;
            o.preview = [c, prec](Vec2 cursor, DisplayList& out) {
                for (const Measurement& m :
                     quickMeasure(c->doc(), entitiesNear(*c, cursor), cursor, prec)) {
                    DisplayPolyline span;
                    span.pts = {m.a, m.b};
                    out.items.emplace_back(std::move(span));
                    DisplayPoints ends;
                    ends.pts = {m.a, m.b};
                    out.items.emplace_back(std::move(ends));
                }
            };
            auto r = co_await ctx.getPoint(
                "Move over the drawing to measure, click to list, Esc to finish", std::move(o));
            if (!r.ok())
                co_return;
            const auto found = quickMeasure(ctx.doc(), entitiesNear(ctx, r.point), r.point, prec);
            if (found.empty()) {
                ctx.message("There is nothing to measure here.");
                continue;
            }
            std::string all;
            for (const Measurement& m : found) {
                ctx.message(m.text);
                all += m.text + "\n";
            }
            ctx.host().setSetting("inquiry.lastresult", all);
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeQuickMeasure() {
    return std::make_unique<QuickMeasure>();
}

}  // namespace tbcad::inquiry
