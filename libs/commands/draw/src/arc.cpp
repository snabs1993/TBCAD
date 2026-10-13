#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

// Counter-clockwise arc through three points in travel order, or the clockwise arc stored as
// counter-clockwise.
std::optional<ArcData> arcThrough(Vec2 a, Vec2 b, Vec2 c) {
    auto r = geom::arcFromThreePoints(a, b, c);
    if (!r)
        return std::nullopt;
    return r.value();
}

// Arc from `start` to `end` that leaves `start` along `tangent`.
std::optional<ArcData> arcTangent(Vec2 start, Vec2 end, double tangent) {
    auto bulge = bulgeFromTangent(start, end, tangent);
    if (!bulge)
        return std::nullopt;
    auto arc = geom::bulgeArc(start, end, *bulge);
    if (!arc)
        return std::nullopt;
    ArcData d;
    d.center = arc->center;
    d.radius = arc->radius;
    d.startAngle = arc->startAngle;
    d.endAngle = normalizeAngle(arc->startAngle + arc->sweep);
    return d;
}

// Arc from `start` to the direction of `end` around `center`, counter-clockwise.
std::optional<ArcData> arcCenterStartEnd(Vec2 center, Vec2 start, Vec2 end) {
    const double a0 = std::atan2(start.y - center.y, start.x - center.x);
    const double a1 = std::atan2(end.y - center.y, end.x - center.x);
    const double sweep = normalizeAngle(a1 - a0);
    if (sweep < 1e-9)
        return std::nullopt;
    return arcBySweep(center, start, sweep);
}

// Arc from start to end sweeping `included` radians.
std::optional<ArcData> arcStartEndIncluded(Vec2 start, Vec2 end, double included) {
    auto c = centerFromIncluded(start, end, included);
    if (!c)
        return std::nullopt;
    return arcBySweep(*c, start, included);
}

// Minor arc from start to end with the given radius, counter-clockwise.
std::optional<ArcData> arcStartEndRadius(Vec2 start, Vec2 end, double radius) {
    const double c = distance(start, end);
    if (!(radius > tol::kLinear) || c < tol::kLinear || c > 2 * radius + tol::kLinear)
        return std::nullopt;
    const double half = std::asin(std::min(1.0, c / (2 * radius)));
    return arcStartEndIncluded(start, end, 2 * half);
}

void previewArc(cmd::CommandContext* c, const std::optional<ArcData>& a, DisplayList& out) {
    if (a)
        previewEntity(*c, makeEntity(*a), out);
}

// ARC [A]: 3 points by default; Start-Center-End/Angle/Length, Start-End-Angle/Direction/Radius
// /Center and Continue (Enter at the first prompt) from the last line or arc.
class ArcCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ARC"; }
    std::vector<std::string> defaultAliases() const override { return {"A"}; }
    std::string_view description() const override { return "Draw an arc"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::PointOptions opts;
        opts.keywords = {"Center"};
        auto first = co_await ctx.getPoint(
            promptWith("Specify start point of arc or Enter to continue", opts.keywords),
            std::move(opts));
        if (first.status == cmd::Status::None) {
            co_await continueLast(ctx);
        } else if (first.status == cmd::Status::Keyword) {
            co_await centerFirst(ctx);
        } else if (first.ok()) {
            co_await fromStart(ctx, first.point);
        }
    }

private:
    static cmd::Task continueLast(cmd::CommandContext& ctx) {
        auto* c = &ctx;
        auto last = lastLineOrArc(ctx.doc());
        if (!last) {
            ctx.message("There is no line or arc to continue from.");
            co_return;
        }
        const Continuation k = *last;
        cmd::PointOptions o;
        o.base = k.point;
        o.allowNone = false;
        o.preview = [c, k](Vec2 cursor, DisplayList& out) {
            previewArc(c, arcTangent(k.point, cursor, k.tangent), out);
        };
        auto end = co_await ctx.getPoint("Specify end point of arc", std::move(o));
        if (!end.ok())
            co_return;
        auto arc = arcTangent(k.point, end.point, k.tangent);
        if (!arc) {
            ctx.message("The end point cannot be reached by an arc tangent to the last object.");
            co_return;
        }
        addEntity(ctx, *arc);
    }

    static cmd::Task fromStart(cmd::CommandContext& ctx, Vec2 start) {
        auto* c = &ctx;
        cmd::PointOptions o;
        o.base = start;
        o.keywords = {"Center", "End"};
        o.allowNone = false;
        o.preview = [c, start](Vec2 cursor, DisplayList& out) {
            previewLine(*c, start, cursor, out);
        };
        auto second = co_await ctx.getPoint(promptWith("Specify second point of arc", o.keywords),
                                            std::move(o));
        if (second.status == cmd::Status::Keyword) {
            if (second.keyword == "Center")
                co_await startCenter(ctx, start);
            else
                co_await startEnd(ctx, start);
            co_return;
        }
        if (!second.ok())
            co_return;
        // 3-point arc
        cmd::PointOptions e;
        e.base = second.point;
        e.allowNone = false;
        e.preview = [c, start, mid = second.point](Vec2 cursor, DisplayList& out) {
            previewArc(c, arcThrough(start, mid, cursor), out);
        };
        auto end = co_await ctx.getPoint("Specify end point of arc", std::move(e));
        if (!end.ok())
            co_return;
        auto arc = arcThrough(start, second.point, end.point);
        if (!arc) {
            ctx.message("The three points are collinear; no arc passes through them.");
            co_return;
        }
        addEntity(ctx, *arc);
    }

    // Center, then start point (Start-Center-... entered centre first).
    static cmd::Task centerFirst(cmd::CommandContext& ctx) {
        auto center = co_await ctx.getPoint("Specify center point of arc");
        if (!center.ok())
            co_return;
        cmd::PointOptions o;
        o.base = center.point;
        o.allowNone = false;
        auto start = co_await ctx.getPoint("Specify start point of arc", std::move(o));
        if (!start.ok())
            co_return;
        co_await centerStartThen(ctx, center.point, start.point);
    }

    static cmd::Task startCenter(cmd::CommandContext& ctx, Vec2 start) {
        cmd::PointOptions o;
        o.base = start;
        o.allowNone = false;
        auto center = co_await ctx.getPoint("Specify center point of arc", std::move(o));
        if (!center.ok())
            co_return;
        co_await centerStartThen(ctx, center.point, start);
    }

    // Both centre and start are known: End point, Angle or chord Length.
    static cmd::Task centerStartThen(cmd::CommandContext& ctx, Vec2 center, Vec2 start) {
        auto* c = &ctx;
        cmd::PointOptions o;
        o.base = center;
        o.allowNone = false;
        o.keywords = {"Angle", "Length"};
        o.preview = [c, center, start](Vec2 cursor, DisplayList& out) {
            previewArc(c, arcCenterStartEnd(center, start, cursor), out);
        };
        auto r =
            co_await ctx.getPoint(promptWith("Specify end point of arc", o.keywords), std::move(o));
        if (r.ok()) {
            auto arc = arcCenterStartEnd(center, start, r.point);
            if (!arc) {
                ctx.message("The end point is on the start direction; no arc can be drawn.");
                co_return;
            }
            addEntity(ctx, *arc);
        } else if (r.status == cmd::Status::Keyword && r.keyword == "Angle") {
            cmd::AngleOptions ao;
            ao.base = center;
            ao.preview = [c, center, start](Vec2 cursor, DisplayList& out) {
                const double a0 = std::atan2(start.y - center.y, start.x - center.x);
                const double a1 = std::atan2(cursor.y - center.y, cursor.x - center.x);
                previewArc(c, arcBySweep(center, start, normalizeAngle(a1 - a0)), out);
            };
            auto ang = co_await ctx.getAngle("Specify included angle", std::move(ao));
            if (!ang.ok())
                co_return;
            auto arc = arcBySweep(center, start, ang.value);
            if (!arc) {
                ctx.message("That included angle cannot be used.");
                co_return;
            }
            addEntity(ctx, *arc);
        } else if (r.status == cmd::Status::Keyword) {  // chord Length
            cmd::DistanceOptions lo;
            lo.allowZero = false;
            auto len = co_await ctx.getDistance("Specify length of chord", std::move(lo));
            if (!len.ok())
                co_return;
            const double radius = distance(center, start);
            if (len.value > 2 * radius + tol::kLinear) {
                ctx.message("The chord is longer than the diameter.");
                co_return;
            }
            const double sweep = 2 * std::asin(std::min(1.0, len.value / (2 * radius)));
            auto arc = arcBySweep(center, start, sweep);
            if (!arc) {
                ctx.message("That chord length cannot be used.");
                co_return;
            }
            addEntity(ctx, *arc);
        }
    }

    // Start and end are known: Center, Angle, Direction or Radius.
    static cmd::Task startEnd(cmd::CommandContext& ctx, Vec2 start) {
        auto* c = &ctx;
        cmd::PointOptions eo;
        eo.base = start;
        eo.allowNone = false;
        eo.preview = [c, start](Vec2 cursor, DisplayList& out) {
            previewLine(*c, start, cursor, out);
        };
        auto endR = co_await ctx.getPoint("Specify end point of arc", std::move(eo));
        if (!endR.ok())
            co_return;
        const Vec2 end = endR.point;
        if (tol::isZeroLength(end - start)) {
            ctx.message("The end point coincides with the start point.");
            co_return;
        }

        cmd::PointOptions o;
        o.base = start;
        o.keywords = {"Angle", "Direction", "Radius"};
        o.allowNone = false;
        o.preview = [c, start, end](Vec2 cursor, DisplayList& out) {
            previewArc(c, arcCenterStartEnd(cursor, start, end), out);
        };
        auto r = co_await ctx.getPoint(promptWith("Specify center point of arc", o.keywords),
                                       std::move(o));
        std::optional<ArcData> arc;
        if (r.ok()) {
            arc = arcCenterStartEnd(r.point, start, end);
        } else if (r.status == cmd::Status::Keyword && r.keyword == "Angle") {
            cmd::AngleOptions ao;
            auto ang = co_await ctx.getAngle("Specify included angle", std::move(ao));
            if (!ang.ok())
                co_return;
            arc = arcStartEndIncluded(start, end, ang.value);
        } else if (r.status == cmd::Status::Keyword && r.keyword == "Direction") {
            cmd::AngleOptions ao;
            ao.base = start;
            ao.preview = [c, start, end](Vec2 cursor, DisplayList& out) {
                previewArc(
                    c, arcTangent(start, end, std::atan2(cursor.y - start.y, cursor.x - start.x)),
                    out);
            };
            auto dir = co_await ctx.getAngle("Specify tangent direction for the start point of arc",
                                             std::move(ao));
            if (!dir.ok())
                co_return;
            arc = arcTangent(start, end, dir.value);
        } else if (r.status == cmd::Status::Keyword) {  // Radius
            cmd::DistanceOptions ro;
            ro.allowZero = false;
            auto rad = co_await ctx.getDistance("Specify radius of arc", std::move(ro));
            if (!rad.ok())
                co_return;
            arc = arcStartEndRadius(start, end, rad.value);
        } else {
            co_return;
        }
        if (!arc) {
            ctx.message("No arc matches those values.");
            co_return;
        }
        addEntity(ctx, *arc);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeArc() {
    return std::make_unique<ArcCommand>();
}

}  // namespace tbcad::draw::detail
