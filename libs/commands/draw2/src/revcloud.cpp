#include <algorithm>
#include <cmath>

#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

constexpr const char* kArcLength = "revcloud.arclength";
constexpr double kDefaultArcLength = 10;
// Each cloud arc sweeps 120 degrees: bulge = tan(sweep / 4).
const double kCloudBulge = std::tan(kPi / 6);

// A closed polyline of arcs along `ring`. With `corners` the ring's own vertices stay cloud
// vertices and every edge is divided into pieces no longer than `arcLength`; otherwise the ring is
// divided into equal pieces by length. The arcs bulge outwards (inwards with `reverse`).
PolylineData cloud(const std::vector<Vec2>& ring, bool corners, double arcLength, bool reverse) {
    std::vector<Vec2> pts;
    const std::size_t n = ring.size();
    if (corners) {
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2 a = ring[i], b = ring[(i + 1) % n];
            const int pieces =
                std::max(1, static_cast<int>(std::ceil(distance(a, b) / arcLength - 1e-9)));
            for (int k = 0; k < pieces; ++k)
                pts.push_back(lerp(a, b, static_cast<double>(k) / pieces));
        }
    } else {
        double total = 0;
        for (std::size_t i = 0; i < n; ++i)
            total += distance(ring[i], ring[(i + 1) % n]);
        const int pieces = std::max(3, static_cast<int>(std::ceil(total / arcLength - 1e-9)));
        const double step = total / pieces;
        std::size_t edge = 0;
        double edgeStart = 0;
        for (int k = 0; k < pieces; ++k) {
            const double s = k * step;
            for (;;) {
                const double len = distance(ring[edge], ring[(edge + 1) % n]);
                if (s <= edgeStart + len + 1e-12 || edge + 1 >= n)
                    break;
                edgeStart += len;
                ++edge;
            }
            const Vec2 a = ring[edge], b = ring[(edge + 1) % n];
            const double len = distance(a, b);
            pts.push_back(len > 0 ? lerp(a, b, std::clamp((s - edgeStart) / len, 0.0, 1.0)) : a);
        }
    }
    double sign = ringArea(ring) >= 0 ? 1.0 : -1.0;
    if (reverse)
        sign = -sign;
    PolylineData d;
    d.closed = true;
    for (const Vec2& p : pts) {
        PolyVertex v;
        v.p = p;
        v.bulge = sign * kCloudBulge;
        d.v.push_back(v);
    }
    return d;
}

struct Ring {
    std::vector<Vec2> pts{};
    bool corners = false;
};

std::optional<Ring> ringOf(const Entity& e) {
    Ring r;
    if (const auto* pl = std::get_if<PolylineData>(&e.data)) {
        if (!pl->closed || pl->v.size() < 3)
            return std::nullopt;
        const bool straight = std::all_of(pl->v.begin(), pl->v.end(),
                                          [](const PolyVertex& v) { return v.bulge == 0; });
        if (straight) {
            for (const auto& v : pl->v)
                r.pts.push_back(v.p);
            r.corners = true;
            return r;
        }
    } else if (kind(e) == EntityKind::Circle || kind(e) == EntityKind::Ellipse ||
               kind(e) == EntityKind::Spline) {
        // handled below
    } else {
        return std::nullopt;
    }
    std::vector<geom::CurveRef> curves;
    if (!geom::curvesOf(e, curves) || curves.empty())
        return std::nullopt;
    for (const auto& c : curves) {
        std::vector<Vec2> pts;
        geom::flatten(c, 0.05, pts);
        if (!pts.empty())
            pts.pop_back();
        r.pts.insert(r.pts.end(), pts.begin(), pts.end());
    }
    if (r.pts.size() < 3)
        return std::nullopt;
    return r;
}

bool cloudable(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Circle:
        case EntityKind::Ellipse:
            return true;
        case EntityKind::Polyline:
        case EntityKind::Spline:
            return ringOf(e).has_value();
        default:
            return false;
    }
}

// REVCLOUD [REVC]: a revision cloud from picked points (Enter closes it) or from a closed object.
// Arclength sets the longest arc chord; Esc draws nothing.
class RevCloudCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "REVCLOUD"; }
    std::vector<std::string> defaultAliases() const override { return {"REVC"}; }
    std::string_view description() const override { return "Draw a revision cloud"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        for (;;) {
            const double len = settingNumber(ctx, kArcLength, kDefaultArcLength);
            cmd::PointOptions o;
            o.keywords = {"Arclength", "Object"};
            o.allowNone = false;
            auto first = co_await ctx.getPoint(
                promptWith("Specify start point (arc length " + std::to_string(len) + ")",
                           o.keywords),
                std::move(o));
            if (first.status == cmd::Status::Keyword) {
                if (first.keyword == "Arclength") {
                    cmd::DistanceOptions lo;
                    lo.defaultValue = len;
                    lo.allowZero = false;
                    auto l =
                        co_await ctx.getDistance("Specify approximate arc length", std::move(lo));
                    if (!l.ok())
                        co_return;
                    saveSetting(ctx, kArcLength, l.value);
                    continue;
                }
                co_await fromObject(ctx, len);
                co_return;
            }
            if (!first.ok())
                co_return;
            co_await fromPoints(ctx, first.point, len);
            co_return;
        }
    }

private:
    static cmd::Task fromPoints(cmd::CommandContext& ctx, Vec2 start, double len) {
        auto* c = &ctx;
        std::vector<Vec2> pts{start};
        for (;;) {
            cmd::PointOptions o;
            o.base = pts.back();
            if (pts.size() > 1)
                o.keywords = {"Undo"};
            o.preview = [c, pts, len](Vec2 cursor, DisplayList& out) {
                std::vector<Vec2> p = pts;
                p.push_back(cursor);
                if (p.size() >= 3)
                    previewEntity(*c, makeEntity(cloud(p, true, len, false)), out);
            };
            auto r = co_await ctx.getPoint(
                promptWith("Specify next point or Enter to close", o.keywords), std::move(o));
            if (r.status == cmd::Status::None)
                break;
            if (r.status == cmd::Status::Keyword) {
                pts.pop_back();
                continue;
            }
            if (!r.ok())
                co_return;
            if (!tol::isZeroLength(r.point - pts.back()))
                pts.push_back(r.point);
        }
        if (pts.size() < 3 || std::abs(ringArea(pts)) < tol::kLinear) {
            ctx.message("A revision cloud needs at least three points that enclose an area.");
            co_return;
        }
        addEntity(ctx, cloud(pts, true, len, false));
    }

    static cmd::Task fromObject(cmd::CommandContext& ctx, double len) {
        cmd::EntityOptions eo;
        eo.filter = cloudable;
        auto e = co_await ctx.getEntity("Select a closed object", std::move(eo));
        if (!e.ok())
            co_return;
        const Entity* ent = ctx.doc().find(e.id);
        auto ring = ent ? ringOf(*ent) : std::nullopt;
        if (!ring) {
            ctx.message("That object cannot be turned into a revision cloud.");
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto rev = co_await ctx.getKeyword("Reverse direction [Yes/No] <No>", std::move(ko));
        if (!rev.ok())
            co_return;
        addEntity(ctx, cloud(ring->pts, ring->corners, len, rev.keyword == "Yes"));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeRevCloud() {
    return std::make_unique<RevCloudCommand>();
}

}  // namespace tbcad::draw2::detail
