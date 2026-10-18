#include <cmath>

#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

Vec2 unit(Vec2 v) {
    const double len = std::hypot(v.x, v.y);
    return len > 0 ? Vec2{v.x / len, v.y / len} : Vec2{1, 0};
}

// The preview shows the line as it would be drawn: very long, so only its direction matters.
cmd::PreviewFn rayPreview(Vec2 from, bool infinite) {
    return [from, infinite](Vec2 cursor, DisplayList& out) {
        if (distance(from, cursor) <= 0)
            return;
        RayData d;
        d.origin = from;
        d.dir = unit(cursor - from);
        d.infinite = infinite;
        DisplayPolyline pl;
        pl.pts = {rayStart(d), rayEnd(d)};
        out.items.push_back(std::move(pl));
    };
}

bool addRay(cmd::CommandContext& ctx, Vec2 origin, Vec2 dir, bool infinite) {
    RayData d;
    d.origin = origin;
    d.dir = unit(dir);
    d.infinite = infinite;
    return addEntity(ctx, d);
}

// RAY: one start point, then any number of rays through points until Enter or Esc.
class RayCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "RAY"; }
    std::string_view description() const override {
        return "Draw a line that starts at a point and goes on forever";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        auto start = co_await ctx.getPoint("Specify start point");
        if (!start.ok())
            co_return;
        for (;;) {
            cmd::PointOptions po;
            po.base = start.point;
            po.preview = rayPreview(start.point, false);
            auto through = co_await ctx.getPoint("Specify through point", po);
            if (!through.ok())
                co_return;
            if (distance(start.point, through.point) <= 0) {
                ctx.message("The through point must differ from the start point.");
                continue;
            }
            ctx.checkpoint();
            addRay(ctx, start.point, through.point - start.point, false);
        }
    }
};

// XLINE [XL]: a line without ends. Specify a point and then points to go through, or one of the
// options for horizontal, vertical, angled, bisecting and offset lines.
class XlineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XLINE"; }
    std::vector<std::string> defaultAliases() const override { return {"XL"}; }
    std::string_view description() const override {
        return "Draw a line that goes on forever both ways";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        cmd::PointOptions po;
        po.keywords = {"Hor", "Ver", "Ang", "Bisect", "Offset"};
        auto first = co_await ctx.getPoint(promptWith("Specify a point", po.keywords), po);
        if (first.status == cmd::Status::Keyword) {
            if (first.keyword == "Hor")
                co_await fixed(ctx, Vec2{1, 0});
            else if (first.keyword == "Ver")
                co_await fixed(ctx, Vec2{0, 1});
            else if (first.keyword == "Ang")
                co_await angled(ctx);
            else if (first.keyword == "Bisect")
                co_await bisect(ctx);
            else
                co_await offset(ctx);
            co_return;
        }
        if (!first.ok())
            co_return;
        for (;;) {
            cmd::PointOptions to;
            to.base = first.point;
            to.preview = rayPreview(first.point, true);
            auto through = co_await ctx.getPoint("Specify through point", to);
            if (!through.ok())
                co_return;
            if (distance(first.point, through.point) <= 0) {
                ctx.message("The through point must differ from the first point.");
                continue;
            }
            ctx.checkpoint();
            addRay(ctx, first.point, through.point - first.point, true);
        }
    }

private:
    // Lines of one fixed direction through each point picked.
    static cmd::Task fixed(cmd::CommandContext& ctx, Vec2 dir) {
        for (;;) {
            cmd::PointOptions po;
            po.preview = [dir](Vec2 cursor, DisplayList& out) {
                RayData d;
                d.origin = cursor;
                d.dir = dir;
                d.infinite = true;
                DisplayPolyline pl;
                pl.pts = {rayStart(d), rayEnd(d)};
                out.items.push_back(std::move(pl));
            };
            auto p = co_await ctx.getPoint("Specify through point", po);
            if (!p.ok())
                co_return;
            ctx.checkpoint();
            addRay(ctx, p.point, dir, true);
        }
    }

    static cmd::Task angled(cmd::CommandContext& ctx) {
        cmd::AngleOptions ao;
        ao.inUcs = true;
        auto a = co_await ctx.getAngle("Enter the angle of the xline", std::move(ao));
        if (!a.ok())
            co_return;
        co_await fixed(ctx, Vec2{std::cos(a.value), std::sin(a.value)});
    }

    // The bisector of the angle at a vertex between two points.
    static cmd::Task bisect(cmd::CommandContext& ctx) {
        auto vertex = co_await ctx.getPoint("Specify angle vertex point");
        if (!vertex.ok())
            co_return;
        cmd::PointOptions po;
        po.base = vertex.point;
        auto s = co_await ctx.getPoint("Specify angle start point", po);
        if (!s.ok())
            co_return;
        for (;;) {
            cmd::PointOptions eo;
            eo.base = vertex.point;
            auto e = co_await ctx.getPoint("Specify angle end point", eo);
            if (!e.ok())
                co_return;
            const Vec2 u = unit(s.point - vertex.point), v = unit(e.point - vertex.point);
            Vec2 dir = u + v;
            if (std::hypot(dir.x, dir.y) < 1e-9)
                dir = {-u.y, u.x};  // opposite directions: the bisector is the perpendicular
            ctx.checkpoint();
            addRay(ctx, vertex.point, dir, true);
        }
    }

    // A line parallel to a picked line, a distance away on the side picked.
    static cmd::Task offset(cmd::CommandContext& ctx) {
        cmd::DistanceOptions dopt;
        auto dist = co_await ctx.getDistance("Specify offset distance", std::move(dopt));
        if (!dist.ok())
            co_return;
        for (;;) {
            cmd::EntityOptions eo;
            eo.filter = [](const Entity& e) {
                return kind(e) == EntityKind::Line || kind(e) == EntityKind::Ray;
            };
            auto pick = co_await ctx.getEntity("Select a line object", std::move(eo));
            if (!pick.ok())
                co_return;
            const Entity* e = ctx.doc().find(pick.id);
            if (!e)
                co_return;
            Vec2 origin, dir;
            if (const auto* l = entityAs<LineData>(*e)) {
                origin = l->a;
                dir = unit(l->b - l->a);
            } else if (const auto* r = entityAs<RayData>(*e)) {
                origin = r->origin;
                dir = r->dir;
            } else {
                continue;
            }
            auto side = co_await ctx.getPoint("Specify side to offset");
            if (!side.ok())
                co_return;
            const Vec2 left{-dir.y, dir.x};
            const double sign = dot(side.point - origin, left) >= 0 ? 1 : -1;
            ctx.checkpoint();
            addRay(ctx, origin + left * (sign * dist.value), dir, true);
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeRay() {
    return std::make_unique<RayCommand>();
}
std::unique_ptr<cmd::Command> makeXline() {
    return std::make_unique<XlineCommand>();
}

}  // namespace tbcad::draw2::detail
