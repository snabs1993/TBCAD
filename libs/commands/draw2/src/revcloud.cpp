#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

constexpr const char* kArcLength = "revcloud.arclength";
constexpr const char* kCalligraphy = "revcloud.calligraphy";
constexpr double kDefaultArcLength = 10;
// Each cloud arc sweeps 120 degrees: bulge = tan(sweep / 4).
const double kCloudBulge = std::tan(kPi / 6);

// More arcs than this would make an object too heavy to draw: the command asks for a longer arc.
constexpr std::size_t kMaxArcs = 20000;

// `ring` without repeated points (a polyline vertex given twice, the last point on the first).
std::vector<Vec2> distinct(const std::vector<Vec2>& ring) {
    std::vector<Vec2> out;
    for (const Vec2& p : ring)
        if (out.empty() || !tol::isZeroLength(p - out.back()))
            out.push_back(p);
    while (out.size() > 1 && tol::isZeroLength(out.front() - out.back()))
        out.pop_back();
    return out;
}

// How many arcs cloud() makes along `ring`.
double arcCount(const std::vector<Vec2>& ring, bool corners, double arcLength) {
    double total = 0, count = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const double d = distance(ring[i], ring[(i + 1) % ring.size()]);
        total += d;
        count += std::max(1.0, std::ceil(d / arcLength - 1e-9));
    }
    return corners ? count : std::max(3.0, std::ceil(total / arcLength - 1e-9));
}

std::string shortNumber(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

// The message when `ring` needs too many arcs, or empty.
std::string tooManyArcs(const std::vector<Vec2>& ring, bool corners, double arcLength) {
    const double n = arcCount(ring, corners, arcLength);
    if (std::isfinite(n) && n <= static_cast<double>(kMaxArcs))
        return {};
    double total = 0;
    for (std::size_t i = 0; i < ring.size(); ++i)
        total += distance(ring[i], ring[(i + 1) % ring.size()]);
    return "The arc length is too small for a cloud this size; use " +
           shortNumber(total / static_cast<double>(kMaxArcs) * 1.01) + " or more.";
}

// A closed polyline of arcs along `ring`. With `corners` the ring's own vertices stay cloud
// vertices and every edge is divided into pieces no longer than `arcLength`; otherwise the ring is
// divided into equal pieces by length. The arcs bulge outwards (inwards with `reverse`).
PolylineData cloud(const std::vector<Vec2>& ringIn, bool corners, double arcLength, bool reverse,
                   bool calligraphy = false) {
    const std::vector<Vec2> ring = distinct(ringIn);
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
        if (calligraphy) {  // each arc swells and thins like a pen stroke
            v.startWidth = 0.12 * arcLength;
            v.endWidth = 0.03 * arcLength;
        }
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
            o.keywords = {"Arclength", "Object", "Rectangular", "Style"};
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
                if (first.keyword == "Style") {
                    cmd::KeywordOptions so;
                    so.keywords = {"Normal", "Calligraphy"};
                    so.defaultKeyword =
                        settingNumber(ctx, kCalligraphy, 0) > 0 ? "Calligraphy" : "Normal";
                    auto st =
                        co_await ctx.getKeyword("Arc style [Normal/Calligraphy]", std::move(so));
                    if (!st.ok())
                        co_return;
                    saveSetting(ctx, kCalligraphy, st.keyword == "Calligraphy" ? 1 : 0);
                    continue;
                }
                if (first.keyword == "Rectangular") {
                    co_await fromRectangle(ctx, len);
                    co_return;
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
        if (distinct(pts).size() < 3 || std::abs(ringArea(pts)) < tol::kLinear) {
            ctx.message("A revision cloud needs at least three points that enclose an area.");
            co_return;
        }
        if (const std::string m = tooManyArcs(pts, true, len); !m.empty()) {
            ctx.message(m);
            co_return;
        }
        addEntity(ctx, cloud(pts, true, len, false, settingNumber(ctx, kCalligraphy, 0) > 0));
    }

    static cmd::Task fromRectangle(cmd::CommandContext& ctx, double len) {
        auto a = co_await ctx.getPoint("Specify first corner point");
        if (!a.ok())
            co_return;
        cmd::PointOptions po;
        po.base = a.point;
        auto b = co_await ctx.getPoint("Specify opposite corner point", std::move(po));
        if (!b.ok())
            co_return;
        if (std::abs(a.point.x - b.point.x) < tol::kLinear ||
            std::abs(a.point.y - b.point.y) < tol::kLinear) {
            ctx.message("The two corners must differ in both directions.");
            co_return;
        }
        const std::vector<Vec2> ring{
            a.point, {b.point.x, a.point.y}, b.point, {a.point.x, b.point.y}};
        if (const std::string m = tooManyArcs(ring, true, len); !m.empty()) {
            ctx.message(m);
            co_return;
        }
        addEntity(ctx, cloud(ring, true, len, false, settingNumber(ctx, kCalligraphy, 0) > 0));
    }

    static cmd::Task fromObject(cmd::CommandContext& ctx, double len) {
        cmd::EntityOptions eo;
        auto* c = &ctx;
        eo.filter = [c](const Entity& x) {
            return cloudable(x) && !c->doc().effective(x.common).locked;
        };
        auto e = co_await ctx.getEntity("Select a closed object", std::move(eo));
        if (!e.ok())
            co_return;
        const Entity* ent = ctx.doc().find(e.id);
        auto ring = ent ? ringOf(*ent) : std::nullopt;
        if (!ring) {
            ctx.message("That object cannot be turned into a revision cloud.");
            co_return;
        }
        if (const std::string m = tooManyArcs(ring->pts, ring->corners, len); !m.empty()) {
            ctx.message(m);
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto rev = co_await ctx.getKeyword("Reverse direction [Yes/No] <No>", std::move(ko));
        if (!rev.ok())
            co_return;
        ent = ctx.doc().find(e.id);
        if (!ent)
            co_return;
        // Like AutoCAD the object is converted: the cloud takes its place and its properties
        // (layer, colour, linetype, ...) and the object is erased.
        Entity cloudEntity{ent->common, cloud(ring->pts, ring->corners, len,
                                              rev.keyword == "Yes",
                                              settingNumber(ctx, kCalligraphy, 0) > 0)};
        cloudEntity.common.id = {};
        auto id = ctx.tx().add(std::move(cloudEntity));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        (void)ctx.tx().erase(e.id);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeRevCloud() {
    return std::make_unique<RevCloudCommand>();
}

}  // namespace tbcad::draw2::detail
