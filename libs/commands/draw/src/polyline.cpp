#include <functional>

#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

// Direction the polyline leaves its last vertex in (radians); 0 before the first segment.
double currentTangent(const std::vector<PolyVertex>& v) {
    if (v.size() < 2)
        return 0;
    const PolyVertex& a = v[v.size() - 2];
    const PolyVertex& b = v.back();
    if (a.bulge == 0) {
        const Vec2 d = b.p - a.p;
        return std::atan2(d.y, d.x);
    }
    return endTangent(a.p, b.p, a.bulge);
}

struct Widths {
    double start = 0, end = 0;
};

// The vertices so far plus a candidate segment to `p`.
std::vector<PolyVertex> withSegment(std::vector<PolyVertex> v, Vec2 p, double bulge, Widths w) {
    v.back().bulge = bulge;
    v.back().startWidth = w.start;
    v.back().endWidth = w.end;
    PolyVertex nv;
    nv.p = p;
    v.push_back(nv);
    return v;
}

Entity polylineEntity(std::vector<PolyVertex> v, bool closed) {
    PolylineData d;
    d.v = std::move(v);
    d.closed = closed;
    return makeEntity(std::move(d));
}

// POLYLINE [PL]: Line and Arc modes, Width/Halfwidth, Close, Undo. Arcs continue tangent to the
// previous segment unless Angle/CEnter/Direction/Radius/Second is chosen. Enter finishes, Esc
// discards the whole polyline.
class PolylineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "POLYLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"PL", "PLINE"}; }
    std::string_view description() const override { return "Draw a polyline of lines and arcs"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto* c = &ctx;
        auto first = co_await ctx.getPoint("Specify start point");
        if (!first.ok())
            co_return;
        std::vector<PolyVertex> v(1);
        v[0].p = first.point;
        bool arcMode = false;
        Widths w;
        bool closed = false;

        for (;;) {
            const bool canClose = v.size() >= (arcMode ? 2u : 3u);
            cmd::PointOptions o;
            o.base = v.back().p;
            if (arcMode) {
                o.keywords = {"Angle", "CEnter"};
                if (canClose)
                    o.keywords.push_back("Close");
                o.keywords.insert(o.keywords.end(), {"Direction", "Halfwidth", "Line", "Radius",
                                                     "Second", "Undo", "Width"});
            } else {
                o.keywords = {"Arc"};
                if (canClose)
                    o.keywords.push_back("Close");
                o.keywords.insert(o.keywords.end(), {"Halfwidth", "Length", "Undo", "Width"});
                if (v.size() < 2)
                    std::erase(o.keywords, "Length");
            }
            if (v.size() < 2)
                std::erase(o.keywords, "Undo");
            o.preview = [c, v, arcMode, w](Vec2 cursor, DisplayList& out) {
                double bulge = 0;
                if (arcMode) {
                    if (auto b = bulgeFromTangent(v.back().p, cursor, currentTangent(v)))
                        bulge = *b;
                }
                previewEntity(*c, polylineEntity(withSegment(v, cursor, bulge, w), false), out);
            };
            auto r = co_await ctx.getPoint(
                promptWith(arcMode ? "Specify endpoint of arc" : "Specify next point", o.keywords),
                std::move(o));

            if (r.status == cmd::Status::None)
                break;
            if (r.status == cmd::Status::Cancel)
                co_return;

            if (r.ok()) {
                std::optional<double> bulge = 0.0;
                if (arcMode) {
                    bulge = bulgeFromTangent(v.back().p, r.point, currentTangent(v));
                    if (!bulge)
                        ctx.message(
                            "That point cannot be reached by an arc tangent to the last segment.");
                }
                if (bulge)
                    addVertex(ctx, v, r.point, *bulge, w);
                continue;
            }

            const std::string& kw = r.keyword;
            if (kw == "Arc") {
                arcMode = true;
            } else if (kw == "Line") {
                arcMode = false;
            } else if (kw == "Length") {
                // A segment of the given length continuing the previous segment's direction.
                cmd::DistanceOptions lo;
                lo.base = v.back().p;
                auto l = co_await ctx.getDistance("Specify length of line", std::move(lo));
                if (!l.ok())
                    co_return;
                const double t = currentTangent(v);
                addVertex(ctx, v, v.back().p + Vec2{std::cos(t), std::sin(t)} * l.value, 0, w);
            } else if (kw == "Undo") {
                v.pop_back();
                v.back().bulge = 0;
                v.back().startWidth = v.back().endWidth = 0;
            } else if (kw == "Width" || kw == "Halfwidth") {
                const double k = kw == "Width" ? 1.0 : 2.0;
                cmd::DistanceOptions so;
                so.defaultValue = w.start / k;
                auto s = co_await ctx.getDistance(
                    kw == "Width" ? "Specify starting width" : "Specify starting half-width",
                    std::move(so));
                if (!s.ok())
                    co_return;
                cmd::DistanceOptions eo;
                eo.defaultValue = s.value;
                auto e = co_await ctx.getDistance(
                    kw == "Width" ? "Specify ending width" : "Specify ending half-width",
                    std::move(eo));
                if (!e.ok())
                    co_return;
                w.start = s.value * k;
                w.end = e.value * k;
            } else if (kw == "Close") {
                std::optional<double> bulge = 0.0;
                if (arcMode)
                    bulge = bulgeFromTangent(v.back().p, v.front().p, currentTangent(v));
                if (!bulge) {
                    ctx.message("The closing arc cannot be tangent to the last segment.");
                    continue;
                }
                v.back().bulge = *bulge;
                v.back().startWidth = w.start;
                v.back().endWidth = w.end;
                closed = true;
                break;
            } else {  // an arc end specified another way
                co_await arcOption(ctx, kw, v, w);
            }
        }

        if (v.size() < 2) {
            ctx.message("A polyline needs at least two points.");
            co_return;
        }
        addEntity(ctx, polylineEntity(std::move(v), closed).data);
    }

private:
    static void addVertex(cmd::CommandContext& ctx, std::vector<PolyVertex>& v, Vec2 p,
                          double bulge, Widths w) {
        if (tol::isZeroLength(p - v.back().p)) {
            ctx.message("The point coincides with the previous vertex.");
            return;
        }
        v = withSegment(std::move(v), p, bulge, w);
    }

    static cmd::Task arcOption(cmd::CommandContext& ctx, std::string kw, std::vector<PolyVertex>& v,
                               Widths w) {
        auto* c = &ctx;
        const Vec2 a = v.back().p;
        const double tangent = currentTangent(v);
        std::optional<double> bulge;
        Vec2 end{};

        cmd::PointOptions po;
        po.base = a;
        po.allowNone = false;
        auto askEnd = [&](const char* text, std::function<std::optional<double>(Vec2)> bulgeAt) {
            po.preview = [c, v, w, bulgeAt](Vec2 cursor, DisplayList& out) {
                auto b = bulgeAt(cursor);
                previewEntity(*c, polylineEntity(withSegment(v, cursor, b.value_or(0), w), false),
                              out);
            };
            return ctx.getPoint(text, po);
        };

        if (kw == "Second") {
            auto s = co_await ctx.getPoint("Specify second point on arc", po);
            if (!s.ok())
                co_return;
            const Vec2 mid = s.point;
            auto e = co_await askEnd("Specify end point of arc",
                                     [a, mid](Vec2 p) { return geom::bulgeThrough(a, mid, p); });
            if (!e.ok())
                co_return;
            end = e.point;
            bulge = geom::bulgeThrough(a, mid, end);
        } else if (kw == "Angle") {
            auto ang = co_await ctx.getAngle("Specify included angle");
            if (!ang.ok())
                co_return;
            const double included = ang.value;
            auto e = co_await askEnd("Specify end point of arc", [a, included](Vec2 p) {
                return bulgeFromIncluded(a, p, included);
            });
            if (!e.ok())
                co_return;
            end = e.point;
            bulge = bulgeFromIncluded(a, end, included);
        } else if (kw == "CEnter") {
            auto ce = co_await ctx.getPoint("Specify center point of arc", po);
            if (!ce.ok())
                co_return;
            const Vec2 center = ce.point;
            auto sweepOf = [a, center](Vec2 p) -> std::optional<double> {
                const double sw = normalizeAngle(std::atan2(p.y - center.y, p.x - center.x) -
                                                 std::atan2(a.y - center.y, a.x - center.x));
                if (sw < 1e-9)
                    return std::nullopt;
                return std::tan(sw / 4);
            };
            auto e = co_await askEnd("Specify end point of arc", sweepOf);
            if (!e.ok())
                co_return;
            end = e.point;
            bulge = sweepOf(end);
        } else if (kw == "Direction") {
            cmd::AngleOptions ao;
            ao.base = a;
            auto dir = co_await ctx.getAngle("Specify tangent direction for the start point of arc",
                                             std::move(ao));
            if (!dir.ok())
                co_return;
            const double d = dir.value;
            auto e = co_await askEnd("Specify end point of arc",
                                     [a, d](Vec2 p) { return bulgeFromTangent(a, p, d); });
            if (!e.ok())
                co_return;
            end = e.point;
            bulge = bulgeFromTangent(a, end, d);
        } else if (kw == "Radius") {
            cmd::DistanceOptions ro;
            ro.allowZero = false;
            auto rad = co_await ctx.getDistance("Specify radius of arc", std::move(ro));
            if (!rad.ok())
                co_return;
            const double radius = rad.value;
            auto radiusBulge = [a, radius, tangent](Vec2 p) -> std::optional<double> {
                const Vec2 chord = p - a;
                const double len = length(chord);
                if (len < tol::kLinear || len > 2 * radius + tol::kLinear)
                    return std::nullopt;
                const double beta = std::asin(std::min(1.0, len / (2 * radius)));
                const double phi = std::atan2(chord.y, chord.x);
                // Of the two minor arcs pick the one that leaves `a` closest to the tangent.
                const double ccw = std::abs(signedAngle(tangent, phi - beta));
                const double cw = std::abs(signedAngle(tangent, phi + beta));
                const double t = std::tan(beta / 2);
                return cw + 1e-12 < ccw ? -t : t;
            };
            auto e = co_await askEnd("Specify end point of arc", radiusBulge);
            if (!e.ok())
                co_return;
            end = e.point;
            bulge = radiusBulge(end);
        } else {
            co_return;
        }
        if (!bulge) {
            ctx.message("No arc matches those values.");
            co_return;
        }
        addVertex(ctx, v, end, *bulge, w);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makePolyline() {
    return std::make_unique<PolylineCommand>();
}

}  // namespace tbcad::draw::detail
