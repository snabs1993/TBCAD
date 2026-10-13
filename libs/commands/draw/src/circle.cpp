#include <limits>

#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

// ---- Tangent-tangent-radius
// ----------------------------------------------------------------------

// A line (infinite) or circle (an arc is treated as its full circle).
struct Obj {
    bool isLine = false;
    Vec2 a{}, b{};  // line
    Vec2 c{};       // circle
    double r = 0;
};

std::optional<Obj> objOf(const Entity& e) {
    Obj o;
    if (const auto* l = std::get_if<LineData>(&e.data)) {
        if (tol::isZeroLength(l->b - l->a))
            return std::nullopt;
        o.isLine = true;
        o.a = l->a;
        o.b = l->b;
        return o;
    }
    if (const auto* c = std::get_if<CircleData>(&e.data)) {
        o.c = c->center;
        o.r = c->radius;
        return o;
    }
    if (const auto* a = std::get_if<ArcData>(&e.data)) {
        o.c = a->center;
        o.r = a->radius;
        return o;
    }
    return std::nullopt;
}

struct Line2 {
    Vec2 p, d;  // point and unit direction
};
struct Circ2 {
    Vec2 c;
    double r;
};

void lineLine(const Line2& l1, const Line2& l2, std::vector<Vec2>& out) {
    const double den = cross(l1.d, l2.d);
    if (std::abs(den) < 1e-12)
        return;
    const double t = cross(l2.p - l1.p, l2.d) / den;
    out.push_back(l1.p + l1.d * t);
}

void lineCircle(const Line2& l, const Circ2& c, std::vector<Vec2>& out) {
    const Vec2 f = l.p - c.c;
    const double b = dot(f, l.d);
    const double disc = b * b - (dot(f, f) - c.r * c.r);
    if (disc < -1e-9)
        return;
    const double s = std::sqrt(std::max(0.0, disc));
    out.push_back(l.p + l.d * (-b + s));
    if (s > 1e-9)
        out.push_back(l.p + l.d * (-b - s));
}

void circleCircle(const Circ2& c1, const Circ2& c2, std::vector<Vec2>& out) {
    const Vec2 dv = c2.c - c1.c;
    const double d = length(dv);
    if (d < 1e-12)
        return;
    const double a = (c1.r * c1.r - c2.r * c2.r + d * d) / (2 * d);
    const double h2 = c1.r * c1.r - a * a;
    if (h2 < -1e-9)
        return;
    const Vec2 u = dv / d;
    const Vec2 m = c1.c + u * a;
    const double h = std::sqrt(std::max(0.0, h2));
    out.push_back(m + perp(u) * h);
    if (h > 1e-9)
        out.push_back(m - perp(u) * h);
}

// The loci of centres of radius-r circles tangent to `o`.
void offsets(const Obj& o, double r, std::vector<Line2>& lines, std::vector<Circ2>& circles) {
    if (o.isLine) {
        const Vec2 u = normalized(o.b - o.a);
        lines.push_back({o.a + perp(u) * r, u});
        lines.push_back({o.a - perp(u) * r, u});
    } else {
        circles.push_back({o.c, o.r + r});
        if (std::abs(o.r - r) > tol::kLinear)
            circles.push_back({o.c, std::abs(o.r - r)});
    }
}

Vec2 tangentPoint(const Obj& o, Vec2 center) {
    if (o.isLine) {
        const Vec2 u = normalized(o.b - o.a);
        return o.a + u * dot(center - o.a, u);
    }
    const Vec2 d = center - o.c;
    return o.c + normalized(d) * o.r;
}

// Centre of the radius-r circle tangent to both objects whose tangent points lie closest to the
// pick points.
std::optional<Vec2> solveTtr(const Obj& o1, const Obj& o2, double r, Vec2 pick1, Vec2 pick2) {
    std::vector<Line2> l1, l2;
    std::vector<Circ2> c1, c2;
    offsets(o1, r, l1, c1);
    offsets(o2, r, l2, c2);
    std::vector<Vec2> cand;
    for (const auto& a : l1) {
        for (const auto& b : l2)
            lineLine(a, b, cand);
        for (const auto& b : c2)
            lineCircle(a, b, cand);
    }
    for (const auto& a : c1) {
        for (const auto& b : l2)
            lineCircle(b, a, cand);
        for (const auto& b : c2)
            circleCircle(a, b, cand);
    }
    std::optional<Vec2> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (const Vec2& c : cand) {
        const double s =
            distance(tangentPoint(o1, c), pick1) + distance(tangentPoint(o2, c), pick2);
        if (s < bestScore - 1e-12) {
            bestScore = s;
            best = c;
        }
    }
    return best;
}

bool isTangentTarget(const Entity& e) {
    return objOf(e).has_value();
}

// CIRCLE [C]: centre + radius/diameter (default), 2P, 3P, Ttr.
class CircleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CIRCLE"; }
    std::vector<std::string> defaultAliases() const override { return {"C"}; }
    std::string_view description() const override { return "Draw a circle"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        cmd::PointOptions opts;
        opts.keywords = {"3P", "2P", "Ttr"};
        auto first = co_await ctx.getPoint(
            promptWith("Specify center point for circle", opts.keywords), std::move(opts));
        if (first.status == cmd::Status::Keyword) {
            if (first.keyword == "3P")
                co_await threePoint(ctx);
            else if (first.keyword == "2P")
                co_await twoPoint(ctx);
            else
                co_await tanTanRadius(ctx);
            co_return;
        }
        if (!first.ok())
            co_return;

        const Vec2 center = first.point;
        cmd::DistanceOptions rad;
        rad.base = center;
        rad.allowZero = false;
        rad.keywords = {"Diameter"};
        rad.preview = [c, center](Vec2 cursor, DisplayList& out) {
            previewEntity(*c, makeEntity(CircleData{center, distance(center, cursor)}), out);
        };
        auto r = co_await ctx.getDistance(promptWith("Specify radius of circle", rad.keywords),
                                          std::move(rad));
        double radius = 0;
        if (r.status == cmd::Status::Keyword) {
            cmd::DistanceOptions dia;
            dia.base = center;
            dia.allowZero = false;
            dia.preview = [c, center](Vec2 cursor, DisplayList& out) {
                previewEntity(*c, makeEntity(CircleData{center, distance(center, cursor) / 2}),
                              out);
            };
            auto d = co_await ctx.getDistance("Specify diameter of circle", std::move(dia));
            if (!d.ok())
                co_return;
            radius = d.value / 2;
        } else if (r.ok()) {
            radius = r.value;
        } else {
            co_return;
        }
        addEntity(ctx, CircleData{center, radius});
    }

private:
    static cmd::Task twoPoint(cmd::CommandContext& ctx) {
        auto* c = &ctx;
        auto p1 = co_await ctx.getPoint("Specify first end point of circle's diameter");
        if (!p1.ok())
            co_return;
        cmd::PointOptions opts;
        opts.base = p1.point;
        opts.preview = [c, a = p1.point](Vec2 cursor, DisplayList& out) {
            previewEntity(*c, makeEntity(CircleData{lerp(a, cursor, 0.5), distance(a, cursor) / 2}),
                          out);
        };
        auto p2 =
            co_await ctx.getPoint("Specify second end point of circle's diameter", std::move(opts));
        if (!p2.ok())
            co_return;
        const double r = distance(p1.point, p2.point) / 2;
        if (!(r > tol::kLinear)) {
            ctx.message("The two points coincide.");
            co_return;
        }
        addEntity(ctx, CircleData{lerp(p1.point, p2.point, 0.5), r});
    }

    static cmd::Task threePoint(cmd::CommandContext& ctx) {
        auto* c = &ctx;
        auto p1 = co_await ctx.getPoint("Specify first point on circle");
        if (!p1.ok())
            co_return;
        cmd::PointOptions o2;
        o2.base = p1.point;
        o2.preview = [c, a = p1.point](Vec2 cursor, DisplayList& out) {
            previewLine(*c, a, cursor, out);
        };
        auto p2 = co_await ctx.getPoint("Specify second point on circle", std::move(o2));
        if (!p2.ok())
            co_return;
        cmd::PointOptions o3;
        o3.base = p2.point;
        o3.preview = [c, a = p1.point, b = p2.point](Vec2 cursor, DisplayList& out) {
            if (auto circ = circleThrough(a, b, cursor))
                previewEntity(*c, makeEntity(*circ), out);
        };
        auto p3 = co_await ctx.getPoint("Specify third point on circle", std::move(o3));
        if (!p3.ok())
            co_return;
        auto circ = circleThrough(p1.point, p2.point, p3.point);
        if (!circ) {
            ctx.message("The three points are collinear; no circle passes through them.");
            co_return;
        }
        addEntity(ctx, *circ);
    }

    static cmd::Task tanTanRadius(cmd::CommandContext& ctx) {
        cmd::EntityOptions eo;
        eo.filter = isTangentTarget;
        auto e1 =
            co_await ctx.getEntity("Select object for first tangent (line, circle or arc)", eo);
        if (!e1.ok())
            co_return;
        auto e2 =
            co_await ctx.getEntity("Select object for second tangent (line, circle or arc)", eo);
        if (!e2.ok())
            co_return;
        cmd::DistanceOptions ro;
        ro.allowZero = false;
        auto r = co_await ctx.getDistance("Specify radius of circle", std::move(ro));
        if (!r.ok())
            co_return;
        const Entity* a = ctx.doc().find(e1.id);
        const Entity* b = ctx.doc().find(e2.id);
        auto oa = a ? objOf(*a) : std::nullopt;
        auto ob = b ? objOf(*b) : std::nullopt;
        if (!oa || !ob) {
            ctx.message("The selected objects cannot be used.");
            co_return;
        }
        auto center = solveTtr(*oa, *ob, r.value, e1.pickPoint, e2.pickPoint);
        if (!center) {
            ctx.message("Circle does not exist.");
            co_return;
        }
        addEntity(ctx, CircleData{*center, r.value});
    }

    static std::optional<CircleData> circleThrough(Vec2 a, Vec2 b, Vec2 c) {
        const double d = 2 * cross(b - a, c - a);
        if (std::abs(d) < 1e-12 * std::max(1.0, lengthSquared(b - a) + lengthSquared(c - a)))
            return std::nullopt;
        const Vec2 ba = b - a, ca = c - a;
        const double bl = lengthSquared(ba), cl = lengthSquared(ca);
        const Vec2 off{(ca.y * bl - ba.y * cl) / d, (ba.x * cl - ca.x * bl) / d};
        CircleData cd;
        cd.center = a + off;
        cd.radius = length(off);
        return cd;
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeCircle() {
    return std::make_unique<CircleCommand>();
}

}  // namespace tbcad::draw::detail
