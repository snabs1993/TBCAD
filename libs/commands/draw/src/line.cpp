#include "common.h"
#include "draw_commands.h"

namespace tbcad::draw::detail {

namespace {

// LINE [L]: chained segments; Close joins the last point to the first, Undo removes the last
// segment, Esc/Enter end the command. Segments already drawn stay (LINE semantics).
class LineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LINE"; }
    std::vector<std::string> defaultAliases() const override { return {"L"}; }
    std::string_view description() const override { return "Draw connected line segments"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        cmd::PointOptions fo;
        fo.allowNone = true;
        auto first = co_await ctx.getPoint("Specify first point or Enter to continue", fo);
        std::vector<Vec2> pts;
        if (first.status == cmd::Status::None) {
            // Enter at the first prompt: start where the last line or arc ended.
            auto last = lastLineOrArc(ctx.doc());
            if (!last) {
                ctx.message("There is no line or arc to continue from.");
                co_return;
            }
            pts.push_back(last->point);
            if (last->fromArc) {
                // After an arc the first segment is tangent to it; ask only for its length.
                cmd::DistanceOptions lo;
                lo.base = last->point;
                auto l = co_await ctx.getDistance("Length of line", std::move(lo));
                if (!l.ok())
                    co_return;
                const Vec2 end =
                    last->point + Vec2{std::cos(last->tangent), std::sin(last->tangent)} * l.value;
                ctx.checkpoint();
                if (!addEntity(ctx, LineData{last->point, end}))
                    co_return;
                pts.push_back(end);
            }
        } else if (first.ok()) {
            pts.push_back(first.point);
        } else {
            co_return;
        }
        for (;;) {
            cmd::PointOptions opts;
            opts.base = pts.back();
            opts.keywords.push_back("Undo");
            if (pts.size() >= 3)
                opts.keywords.push_back("Close");
            auto* c = &ctx;
            opts.preview = [c, from = pts.back()](Vec2 cursor, DisplayList& out) {
                previewLine(*c, from, cursor, out);
            };
            auto r = co_await ctx.getPoint(promptWith("Specify next point", opts.keywords),
                                           std::move(opts));
            if (r.status == cmd::Status::Keyword) {
                if (r.keyword == "Undo") {
                    if (pts.size() > 1 && ctx.rollbackToCheckpoint())
                        pts.pop_back();
                    else
                        ctx.message("Nothing to undo.");
                } else {  // Close
                    ctx.checkpoint();
                    addEntity(ctx, LineData{pts.back(), pts.front()});
                    co_return;
                }
                continue;
            }
            if (!r.ok())
                co_return;
            ctx.checkpoint();
            if (addEntity(ctx, LineData{pts.back(), r.point}))
                pts.push_back(r.point);
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeLine() {
    return std::make_unique<LineCommand>();
}

}  // namespace tbcad::draw::detail
