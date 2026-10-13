#include <cmath>

#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

constexpr const char* kIncrementKey = "sketch.increment";

// SKETCH [SK]: freehand drawing. Click to put the pen down, move the cursor to draw, click again
// to lift it; Enter keeps the strokes as polylines. The pen records a point whenever the cursor
// has moved by the increment.
class SketchCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SKETCH"; }
    std::vector<std::string> defaultAliases() const override { return {"SK"}; }
    std::string_view description() const override { return "Draw freehand lines"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        double inc = settingNumber(ctx, kIncrementKey, 1.0);
        std::vector<std::vector<Vec2>> strokes;
        std::vector<Vec2> live;  // the stroke being drawn
        bool pen = false;
        for (;;) {
            cmd::PointOptions po;
            po.keywords = {"Increment", "Accept", "Discard"};
            po.preview = [&](Vec2 cursor, DisplayList& out) {
                if (pen && !live.empty() && distance(live.back(), cursor) >= inc)
                    live.push_back(cursor);
                auto show = [&](const std::vector<Vec2>& pts) {
                    if (pts.size() < 2)
                        return;
                    DisplayPolyline pl;
                    pl.pts = pts;
                    out.items.push_back(std::move(pl));
                };
                for (const auto& s : strokes)
                    show(s);
                show(live);
            };
            auto p = co_await ctx.getPoint(promptWith(pen ? "Pen down, move to draw, click to lift"
                                                          : "Pen up, click to put it down",
                                                      po.keywords),
                                           po);
            if (p.status == cmd::Status::Keyword) {
                if (p.keyword == "Increment") {
                    cmd::RealOptions ro;
                    ro.min = 1e-6;
                    ro.defaultValue = inc;
                    auto r = co_await ctx.getReal("Record increment", ro);
                    if (r.ok()) {
                        inc = r.value;
                        saveSetting(ctx, kIncrementKey, inc);
                    }
                    continue;
                }
                if (p.keyword == "Discard")
                    co_return;
                break;  // Accept
            }
            if (p.status == cmd::Status::None)
                break;  // Enter accepts
            if (!p.ok())
                co_return;  // Esc discards
            if (!pen) {
                live.assign(1, p.point);
                pen = true;
            } else {
                if (distance(live.back(), p.point) > 0)
                    live.push_back(p.point);
                if (live.size() >= 2)
                    strokes.push_back(std::move(live));
                live.clear();
                pen = false;
            }
        }
        if (pen && live.size() >= 2)
            strokes.push_back(std::move(live));
        if (strokes.empty())
            co_return;
        ctx.checkpoint();
        for (const auto& s : strokes) {
            PolylineData d;
            for (const Vec2& pt : s) {
                PolyVertex v;
                v.p = pt;
                d.v.push_back(v);
            }
            addEntity(ctx, std::move(d));
        }
        ctx.message(std::to_string(strokes.size()) + (strokes.size() == 1 ? " line" : " lines") +
                    " recorded.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSketch() {
    return std::make_unique<SketchCommand>();
}

}  // namespace tbcad::draw2::detail
