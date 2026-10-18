#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

constexpr const char* kTraceWidthKey = "trace.width";

EntityData traceSegment(Vec2 a, Vec2 b, double width) {
    PolylineData d;
    d.v.resize(2);
    d.v[0].p = a;
    d.v[1].p = b;
    d.v[0].startWidth = d.v[0].endWidth = width;
    d.v[1].startWidth = d.v[1].endWidth = width;
    return d;
}

// TRACE: wide line segments. Each segment is an open polyline of constant width, so it plots
// filled and edits like any polyline.
class TraceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TRACE"; }
    std::string_view description() const override { return "Draw wide line segments"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        double width = 0.05;
        if (auto s = ctx.host().setting(kTraceWidthKey)) {
            const double w = std::atof(s->c_str());
            if (w > 0)
                width = w;
        }
        cmd::DistanceOptions wo;
        wo.defaultValue = width;
        wo.allowZero = false;
        auto w = co_await ctx.getDistance("Specify trace width", std::move(wo));
        if (!w.ok())
            co_return;
        width = w.value;
        ctx.host().setSetting(kTraceWidthKey, std::to_string(width));
        auto first = co_await ctx.getPoint("Specify start point");
        if (!first.ok())
            co_return;
        Vec2 from = first.point;
        for (;;) {
            cmd::PointOptions opts;
            opts.base = from;
            auto* c = &ctx;
            opts.preview = [c, from, width](Vec2 cursor, DisplayList& out) {
                if (length(cursor - from) > tol::kLinear)
                    previewEntity(*c, makeEntity(traceSegment(from, cursor, width)), out);
            };
            auto r = co_await ctx.getPoint("Specify next point", std::move(opts));
            if (!r.ok())
                co_return;
            if (length(r.point - from) <= tol::kLinear)
                continue;
            ctx.checkpoint();
            if (addEntity(ctx, traceSegment(from, r.point, width)))
                from = r.point;
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeTrace() {
    return std::make_unique<TraceCommand>();
}

}  // namespace tbcad::draw::detail
