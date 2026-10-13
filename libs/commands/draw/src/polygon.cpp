#include <algorithm>

#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

constexpr const char* kSidesSetting = "polygon.sides";

std::vector<PolyVertex> regular(Vec2 center, double radius, double firstAngle, int sides) {
    std::vector<PolyVertex> v(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i) {
        const double a = firstAngle + kTwoPi * i / sides;
        v[static_cast<std::size_t>(i)].p = center + Vec2{std::cos(a), std::sin(a)} * radius;
    }
    return v;
}

// Polygon whose first edge runs a -> b with the interior on the left.
std::vector<PolyVertex> fromEdge(Vec2 a, Vec2 b, int sides) {
    const Vec2 chord = b - a;
    const double s = length(chord);
    const Vec2 u = chord / s;
    const double apothem = (s / 2) / std::tan(kPi / sides);
    const Vec2 center = lerp(a, b, 0.5) + perp(u) * apothem;
    const double radius = (s / 2) / std::sin(kPi / sides);
    return regular(center, radius, std::atan2(a.y - center.y, a.x - center.x), sides);
}

Entity polygonEntity(std::vector<PolyVertex> v) {
    PolylineData d;
    d.v = std::move(v);
    d.closed = true;
    return makeEntity(std::move(d));
}

// POLYGON [POL]: 3 to 1024 sides, from a centre (Inscribed or Circumscribed) or from one Edge.
// The point that defines the radius also sets the orientation: an inscribed polygon has a vertex
// there, a circumscribed one the middle of an edge.
class PolygonCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "POLYGON"; }
    std::vector<std::string> defaultAliases() const override { return {"POL"}; }
    std::string_view description() const override { return "Draw a regular polygon"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        long long defSides = 4;
        if (auto s = ctx.host().setting(kSidesSetting)) {
            try {
                defSides = std::clamp(std::stoll(*s), 3LL, 1024LL);
            } catch (...) {
            }
        }
        cmd::IntOptions io;
        io.defaultValue = defSides;
        io.min = 3;
        io.max = 1024;
        auto n = co_await ctx.getInteger("Enter number of sides <" + std::to_string(defSides) + ">",
                                         std::move(io));
        if (!n.ok())
            co_return;
        const int sides = static_cast<int>(n.value);

        cmd::PointOptions co;
        co.keywords = {"Edge"};
        co.allowNone = false;
        auto first = co_await ctx.getPoint(promptWith("Specify center of polygon", co.keywords),
                                           std::move(co));
        if (first.status == cmd::Status::Keyword) {
            auto p1 = co_await ctx.getPoint("Specify first endpoint of edge");
            if (!p1.ok())
                co_return;
            cmd::PointOptions o2;
            o2.base = p1.point;
            o2.allowNone = false;
            o2.preview = [c, a = p1.point, sides](Vec2 cursor, DisplayList& out) {
                if (!tol::isZeroLength(cursor - a))
                    previewEntity(*c, polygonEntity(fromEdge(a, cursor, sides)), out);
            };
            auto p2 = co_await ctx.getPoint("Specify second endpoint of edge", std::move(o2));
            if (!p2.ok())
                co_return;
            if (tol::isZeroLength(p2.point - p1.point)) {
                ctx.message("The edge has zero length.");
                co_return;
            }
            finish(ctx, fromEdge(p1.point, p2.point, sides), sides);
            co_return;
        }
        if (!first.ok())
            co_return;
        const Vec2 center = first.point;

        cmd::KeywordOptions ko;
        ko.keywords = {"Inscribed", "Circumscribed"};
        ko.defaultKeyword = "Inscribed";
        auto mode = co_await ctx.getKeyword(
            "Enter an option [Inscribed in circle/Circumscribed about circle] <Inscribed>",
            std::move(ko));
        if (!mode.ok())
            co_return;
        const bool inscribed = mode.keyword == "Inscribed";

        // Orientation comes from the clicked point; a typed radius leaves it at 0 degrees.
        auto build = [sides, center, inscribed](
                         double r, double phi) -> std::optional<std::vector<PolyVertex>> {
            if (r < tol::kLinear)
                return std::nullopt;
            if (inscribed)
                return regular(center, r, phi, sides);
            return regular(center, r / std::cos(kPi / sides), phi - kPi / sides, sides);
        };
        auto angleOf = [center](Vec2 p) {
            return tol::isZeroLength(p - center) ? 0.0 : std::atan2(p.y - center.y, p.x - center.x);
        };
        cmd::DistanceOptions ro;
        ro.base = center;
        ro.allowZero = false;
        ro.preview = [c, build, angleOf, center](Vec2 cursor, DisplayList& out) {
            if (auto v = build(distance(center, cursor), angleOf(cursor)))
                previewEntity(*c, polygonEntity(std::move(*v)), out);
        };
        auto rp = co_await ctx.getDistance(inscribed ? "Specify radius of circle (inscribed)"
                                                     : "Specify radius of circle (circumscribed)",
                                           std::move(ro));
        if (!rp.ok())
            co_return;
        auto v = build(rp.value, angleOf(ctx.lastPoint()));
        if (!v) {
            ctx.message("The radius must not be zero.");
            co_return;
        }
        finish(ctx, std::move(*v), sides);
    }

private:
    static void finish(cmd::CommandContext& ctx, std::vector<PolyVertex> v, int sides) {
        if (addEntity(ctx, polygonEntity(std::move(v)).data))
            ctx.host().setSetting(kSidesSetting, std::to_string(sides));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePolygon() {
    return std::make_unique<PolygonCommand>();
}

}  // namespace tbcad::draw::detail
