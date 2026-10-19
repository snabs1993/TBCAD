#include <algorithm>
#include <cmath>
#include <string>

#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

using cmd::CommandContext;

// WIPEOUT [WO]: a closed shape that hides what is behind it (drawn earlier), for text on top of
// hatches or clutter. Pick points (Enter closes), give a Rectangle, or turn a closed Polyline
// into a wipeout. Remove turns wipeouts back into ordinary polylines.
class WipeoutCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "WIPEOUT"; }
    std::vector<std::string> defaultAliases() const override { return {"WO"}; }
    std::string_view description() const override { return "Hide what is behind a closed shape"; }

    cmd::Task run(CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Polygon", "Rectangle", "Polyline", "Remove", "Frames"};
        ko.defaultKeyword = "Polygon";
        auto k = co_await ctx.getKeyword(
            "Wipeout [Polygon/Rectangle/Polyline/Remove/Frames] <Polygon>", ko);
        if (!k.ok())
            co_return;

        if (k.keyword == "Frames") {  // as WIPEOUTFRAME, by name like AutoCAD's Frames option
            cmd::KeywordOptions fo;
            fo.keywords = {"On", "Off", "Display"};
            const int now = wipeoutFrameMode(ctx.doc().header());
            fo.defaultKeyword = now == 0 ? "Off" : now == 2 ? "Display" : "On";
            auto f = co_await ctx.getKeyword(
                "Wipeout frames [On/Off/Display] <" + *fo.defaultKeyword + ">", fo);
            if (!f.ok())
                co_return;
            const std::string value = f.keyword == "Off" ? "0" : f.keyword == "Display" ? "2" : "1";
            (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kWipeoutFrameKey] = value; });
            ctx.setUndoLabel("WIPEOUTFRAME");
            ctx.message("WIPEOUTFRAME = " + value);
            co_return;
        }

        if (k.keyword == "Polyline" || k.keyword == "Remove") {
            const bool remove = k.keyword == "Remove";
            auto sel = co_await ctx.getSelection(
                remove ? "Select wipeouts" : "Select closed polylines",
                cmd::SelectionOptions{.filter = [remove, c = &ctx](const Entity& e) {
                    const auto* p = entityAs<PolylineData>(e);
                    return p && p->closed && e.common.wipeout == remove &&
                           !c->doc().effective(e.common).locked;
                }});
            if (!sel.ok() || sel.ids.empty())
                co_return;
            std::size_t n = 0;
            for (EntityId id : sel.ids)
                if (ctx.tx().modify(id, [remove](Entity& x) { x.common.wipeout = !remove; }))
                    ++n;
            ctx.message(std::to_string(n) + (remove ? " no longer hide" : " now hide") +
                        " what is behind");
            ctx.setUndoLabel("Wipeout");
            co_return;
        }

        std::vector<Vec2> pts;
        if (k.keyword == "Rectangle") {
            auto a = co_await ctx.getPoint("First corner");
            if (!a.ok())
                co_return;
            cmd::PointOptions po;
            po.base = a.point;
            auto b = co_await ctx.getPoint("Opposite corner", po);
            if (!b.ok())
                co_return;
            const Vec2 p = a.point, q = b.point;
            if (std::abs(p.x - q.x) < tol::kLinear || std::abs(p.y - q.y) < tol::kLinear) {
                ctx.message("The corners must differ in both directions.");
                co_return;
            }
            pts = {p, {q.x, p.y}, q, {p.x, q.y}};
        } else {
            for (;;) {
                cmd::PointOptions po;
                po.allowNone = pts.size() >= 3;
                if (!pts.empty())
                    po.base = pts.back();
                if (!pts.empty())
                    po.keywords = {"Undo"};
                auto a = co_await ctx.getPoint(
                    pts.size() >= 3 ? "Next point or [Undo] (Enter closes)" : "Next point", po);
                if (a.status == cmd::Status::Keyword) {
                    pts.pop_back();
                    continue;
                }
                if (!a.ok()) {
                    if (a.status == cmd::Status::None && pts.size() >= 3)
                        break;
                    co_return;
                }
                // A point on the last one adds nothing.
                if (pts.empty() || !tol::isZeroLength(a.point - pts.back()))
                    pts.push_back(a.point);
            }
            while (pts.size() > 1 && tol::isZeroLength(pts.front() - pts.back()))
                pts.pop_back();
            if (pts.size() < 3 || std::abs(ringArea(pts)) < tol::kLinear) {
                ctx.message("A wipeout needs at least three points that enclose an area.");
                co_return;
            }
        }
        PolylineData pl;
        pl.closed = true;
        for (Vec2 p : pts)
            pl.v.push_back(PolyVertex{.p = p});
        Entity e;
        e.data = std::move(pl);
        applyCurrentProperties(ctx.doc().header(), e);
        e.common.wipeout = true;
        auto id = ctx.tx().add(std::move(e));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        ctx.setUndoLabel("Wipeout");
    }
};

// WIPEOUTFRAME: 0 hides the outlines of wipeouts, 1 shows and plots them, 2 shows them on screen
// only. It is a setting of the drawing.
class WipeoutFrameCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "WIPEOUTFRAME"; }
    std::vector<std::string> defaultAliases() const override { return {"WFRAME"}; }
    std::string_view description() const override {
        return "Show, hide or plot the outlines of wipeouts";
    }

    cmd::Task run(CommandContext& ctx) override {
        const int now = wipeoutFrameMode(ctx.doc().header());
        cmd::IntOptions io;
        io.defaultValue = now;
        io.min = 0;
        io.max = 2;
        auto v = co_await ctx.getInteger(
            "WIPEOUTFRAME 0 hidden, 1 shown and plotted, 2 shown not "
            "plotted <" +
                std::to_string(now) + ">",
            io);
        if (!v.ok())
            co_return;
        const std::string value = std::to_string(std::clamp<long long>(v.value, 0, 2));
        (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[kWipeoutFrameKey] = value; });
        ctx.setUndoLabel("WIPEOUTFRAME");
        ctx.message("WIPEOUTFRAME = " + value);
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeWipeoutFrame() {
    return std::make_unique<WipeoutFrameCommand>();
}

std::unique_ptr<cmd::Command> makeWipeout() {
    return std::make_unique<WipeoutCommand>();
}

}  // namespace tbcad::draw2::detail
