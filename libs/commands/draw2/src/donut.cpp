#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

constexpr const char* kInner = "donut.inner";
constexpr const char* kOuter = "donut.outer";

// A ring of width (outer - inner) / 2 along a circle of radius (outer + inner) / 4: two half
// circle segments of a closed polyline. A zero inner diameter gives a solid disc.
std::optional<PolylineData> donut(Vec2 c, double inner, double outer) {
    if (!(outer > inner) || inner < 0)
        return std::nullopt;
    const double w = (outer - inner) / 2;
    const double r = (outer + inner) / 4;
    PolylineData d;
    d.closed = true;
    d.constantWidth = w;
    d.v.resize(2);
    d.v[0].p = {c.x - r, c.y};
    d.v[1].p = {c.x + r, c.y};
    for (auto& v : d.v) {
        v.bulge = 1;
        v.startWidth = v.endWidth = w;
    }
    return d;
}

// DONUT [DO]: filled rings; diameters are remembered, centres are asked for until Enter.
class DonutCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DONUT"; }
    std::vector<std::string> defaultAliases() const override { return {"DO"}; }
    std::string_view description() const override { return "Draw filled circles and rings"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        ctx.commitOnCancel();
        cmd::DistanceOptions io;
        io.defaultValue = settingNumber(ctx, kInner, 5);
        auto inner = co_await ctx.getDistance("Specify inside diameter of donut", std::move(io));
        if (!inner.ok())
            co_return;
        double outer = 0;
        for (;;) {
            cmd::DistanceOptions oo;
            oo.defaultValue = settingNumber(ctx, kOuter, 10);
            oo.allowZero = false;
            auto o = co_await ctx.getDistance("Specify outside diameter of donut", std::move(oo));
            if (!o.ok())
                co_return;
            if (o.value > inner.value) {
                outer = o.value;
                break;
            }
            ctx.message("The outside diameter must be larger than the inside diameter.");
        }
        saveSetting(ctx, kInner, inner.value);
        saveSetting(ctx, kOuter, outer);

        const double in = inner.value;
        for (;;) {
            cmd::PointOptions po;
            po.preview = [c, in, outer](Vec2 cursor, DisplayList& out) {
                if (auto d = donut(cursor, in, outer))
                    previewEntity(*c, makeEntity(std::move(*d)), out);
            };
            auto r = co_await ctx.getPoint("Specify center of donut or exit", std::move(po));
            if (!r.ok())
                co_return;
            ctx.checkpoint();
            if (auto d = donut(r.point, in, outer))
                addEntity(ctx, std::move(*d));
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeDonut() {
    return std::make_unique<DonutCommand>();
}

}  // namespace tbcad::draw2::detail
