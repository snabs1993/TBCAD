#include <sstream>

#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

constexpr const char* kFilletKey = "rectangle.fillet";
constexpr const char* kChamferKey = "rectangle.chamfer";  // "d1 d2"
constexpr const char* kWidthKey = "rectangle.width";

double readNumber(const cmd::IHost& host, const char* key, double fallback = 0) {
    if (auto v = host.setting(key)) {
        try {
            return std::stod(*v);
        } catch (...) {
        }
    }
    return fallback;
}

struct RectSpec {
    double rotation = 0;
    double fillet = 0;                  // corner radius; wins over the chamfer when both are set
    double chamfer1 = 0, chamfer2 = 0;  // distances along the edges that meet at a corner
    double width = 0;                   // polyline width

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
    const Vec2 corner[4] = {c1, c1 + u * dx, c1 + u * dx + v * dy, c1 + v * dy};
    // Rounded and chamfered corners stay inside the shorter side.
    const double room = std::min(std::abs(dx), std::abs(dy)) / 2;
    const double r = std::min(spec.fillet, room);
    const double c1d = std::min(spec.chamfer1, room), c2d = std::min(spec.chamfer2, room);
    std::vector<PolyVertex> out;
    if (r > tol::kLinear || c1d > tol::kLinear || c2d > tol::kLinear) {
        // Counter-clockwise when the two signs agree; convex arcs then bulge to the right.
        const double bulge = std::tan(M_PI / 8) * (dx * dy > 0 ? 1.0 : -1.0);
        for (int i = 0; i < 4; ++i) {
            const Vec2 prev = corner[(i + 3) % 4], next = corner[(i + 1) % 4];
            const Vec2 toPrev = (prev - corner[i]) / length(prev - corner[i]);
            const Vec2 toNext = (next - corner[i]) / length(next - corner[i]);
            PolyVertex a, b;
            a.p = corner[i] + toPrev * (r > tol::kLinear ? r : c1d);
            b.p = corner[i] + toNext * (r > tol::kLinear ? r : c2d);
            if (r > tol::kLinear)
                a.bulge = bulge;
            out.push_back(a);
            out.push_back(b);
        }
    } else {
        out.resize(4);
        for (int i = 0; i < 4; ++i)
            out[static_cast<std::size_t>(i)].p = corner[i];
    }
    for (PolyVertex& pv : out)
        pv.startWidth = pv.endWidth = spec.width;
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
        spec.fillet = readNumber(ctx.host(), kFilletKey);
        spec.width = readNumber(ctx.host(), kWidthKey);
        if (auto ch = ctx.host().setting(kChamferKey)) {
            std::istringstream in(*ch);
            in >> spec.chamfer1 >> spec.chamfer2;
        }
        for (;;) {
            cmd::PointOptions o;
            o.base = c1;
            o.allowNone = false;
            o.keywords = {"Dimensions", "Rotation", "Fillet", "Chamfer", "Width"};
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
                } else if (r.keyword == "Fillet") {
                    cmd::DistanceOptions fo;
                    fo.defaultValue = spec.fillet;
                    auto f = co_await ctx.getDistance("Specify fillet radius for rectangles", fo);
                    if (!f.ok())
                        co_return;
                    spec.fillet = std::max(0.0, f.value);
                    spec.chamfer1 = spec.chamfer2 = 0;
                    ctx.host().setSetting(kFilletKey, std::to_string(spec.fillet));
                    ctx.host().setSetting(kChamferKey, "0 0");
                } else if (r.keyword == "Chamfer") {
                    cmd::DistanceOptions d1;
                    d1.defaultValue = spec.chamfer1;
                    auto a = co_await ctx.getDistance("Specify first chamfer distance", d1);
                    if (!a.ok())
                        co_return;
                    cmd::DistanceOptions d2;
                    d2.defaultValue = a.value;
                    auto b = co_await ctx.getDistance("Specify second chamfer distance", d2);
                    if (!b.ok())
                        co_return;
                    spec.chamfer1 = std::max(0.0, a.value);
                    spec.chamfer2 = std::max(0.0, b.value);
                    spec.fillet = 0;
                    ctx.host().setSetting(kFilletKey, "0");
                    ctx.host().setSetting(kChamferKey, std::to_string(spec.chamfer1) + " " +
                                                           std::to_string(spec.chamfer2));
                } else if (r.keyword == "Width") {
                    cmd::DistanceOptions wo2;
                    wo2.defaultValue = spec.width;
                    auto w = co_await ctx.getDistance("Specify line width for rectangles", wo2);
                    if (!w.ok())
                        co_return;
                    spec.width = std::max(0.0, w.value);
                    ctx.host().setSetting(kWidthKey, std::to_string(spec.width));
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
