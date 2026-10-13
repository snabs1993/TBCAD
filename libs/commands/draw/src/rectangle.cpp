#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

struct RectSpec {
    double rotation = 0;
    std::optional<std::pair<double, double>> dims{};  // length (along u), width (along v)
};

// Corners of the rectangle that starts at `c1`, is rotated by spec.rotation and contains `c2`
// (or, with fixed dimensions, extends towards `c2`).
std::optional<std::vector<PolyVertex>> corners(Vec2 c1, Vec2 c2, const RectSpec& spec) {
    const Vec2 u{std::cos(spec.rotation), std::sin(spec.rotation)};
    const Vec2 v = perp(u);
    const Vec2 d = c2 - c1;
    double dx = dot(d, u), dy = dot(d, v);
    if (spec.dims) {
        dx = (dx < 0 ? -1.0 : 1.0) * spec.dims->first;
        dy = (dy < 0 ? -1.0 : 1.0) * spec.dims->second;
    }
    if (std::abs(dx) < tol::kLinear || std::abs(dy) < tol::kLinear)
        return std::nullopt;
    std::vector<PolyVertex> out(4);
    out[0].p = c1;
    out[1].p = c1 + u * dx;
    out[2].p = c1 + u * dx + v * dy;
    out[3].p = c1 + v * dy;
    return out;
}

Entity rectEntity(std::vector<PolyVertex> v) {
    PolylineData d;
    d.v = std::move(v);
    d.closed = true;
    return makeEntity(std::move(d));
}

// RECTANGLE [REC]: two corners -> closed 4-vertex polyline. Dimensions (length, width, then a
// point choosing the side) and Rotation (angle of the first edge) are options of the second
// corner prompt.
class RectangleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "RECTANGLE"; }
    std::vector<std::string> defaultAliases() const override { return {"REC"}; }
    std::string_view description() const override { return "Draw a rectangle"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        auto first = co_await ctx.getPoint("Specify first corner point");
        if (!first.ok())
            co_return;
        const Vec2 c1 = first.point;
        RectSpec spec;
        for (;;) {
            cmd::PointOptions o;
            o.base = c1;
            o.allowNone = false;
            o.keywords = {"Dimensions", "Rotation"};
            o.preview = [c, c1, spec](Vec2 cursor, DisplayList& out) {
                if (auto v = corners(c1, cursor, spec))
                    previewEntity(*c, rectEntity(std::move(*v)), out);
            };
            auto r = co_await ctx.getPoint(promptWith("Specify other corner point", o.keywords),
                                           std::move(o));
            if (r.status == cmd::Status::Keyword) {
                if (r.keyword == "Rotation") {
                    cmd::AngleOptions ao;
                    ao.base = c1;
                    ao.defaultValue = spec.rotation;
                    auto a = co_await ctx.getAngle("Specify rotation angle", std::move(ao));
                    if (!a.ok())
                        co_return;
                    spec.rotation = a.value;
                } else {
                    cmd::DistanceOptions lo;
                    lo.allowZero = false;
                    auto len =
                        co_await ctx.getDistance("Specify length for rectangles", std::move(lo));
                    if (!len.ok())
                        co_return;
                    cmd::DistanceOptions wo;
                    wo.allowZero = false;
                    auto wid =
                        co_await ctx.getDistance("Specify width for rectangles", std::move(wo));
                    if (!wid.ok())
                        co_return;
                    spec.dims = std::make_pair(len.value, wid.value);
                }
                continue;
            }
            if (!r.ok())
                co_return;
            auto v = corners(c1, r.point, spec);
            if (!v) {
                ctx.message("The rectangle would have zero width or height.");
                continue;
            }
            addEntity(ctx, rectEntity(std::move(*v)).data);
            co_return;
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeRectangle() {
    return std::make_unique<RectangleCommand>();
}

}  // namespace tbcad::draw::detail
