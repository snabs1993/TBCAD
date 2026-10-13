#include "commands.h"
#include "common.h"

namespace tbcad::draw2::detail {

namespace {

HatchData solidOf(const std::vector<Vec2>& ring) {
    HatchPolylineEdge edge;
    edge.closed = true;
    for (Vec2 p : ring) {
        PolyVertex v;
        v.p = p;
        edge.v.push_back(v);
    }
    HatchLoop loop;
    loop.outer = true;
    loop.edges.emplace_back(std::move(edge));
    HatchData h;
    h.solid = true;
    h.loops.push_back(std::move(loop));
    return h;
}

// Corners are given in the "bow-tie" order 1, 2, 3, 4 (as in AutoCAD): the outline runs 1, 2, 4, 3.
std::vector<Vec2> outline(Vec2 p1, Vec2 p2, Vec2 p3, std::optional<Vec2> p4) {
    if (!p4)
        return {p1, p2, p3};
    return {p1, p2, *p4, p3};
}

// SOLID [SO]: a filled triangle or quadrilateral. After the fourth point the command asks for
// more third and fourth points, which continue from the edge just drawn; Enter ends it.
class SolidCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SOLID"; }
    std::vector<std::string> defaultAliases() const override { return {"SO"}; }
    std::string_view description() const override {
        return "Draw a filled triangle or four-sided shape";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        auto a = co_await ctx.getPoint("Specify first point");
        if (!a.ok())
            co_return;
        cmd::PointOptions po;
        po.base = a.point;
        auto b = co_await ctx.getPoint("Specify second point", po);
        if (!b.ok())
            co_return;
        Vec2 p1 = a.point, p2 = b.point;
        for (;;) {
            cmd::PointOptions o3;
            o3.base = p2;
            auto* c = &ctx;
            o3.preview = [c, p1, p2](Vec2 cur, DisplayList& out) {
                previewEntity(*c, makeEntity(solidOf(outline(p1, p2, cur, std::nullopt))), out);
            };
            auto third = co_await ctx.getPoint("Specify third point", o3);
            if (!third.ok())
                co_return;
            cmd::PointOptions o4;
            o4.base = third.point;
            o4.allowNone = true;
            Vec2 p3 = third.point;
            o4.preview = [c, p1, p2, p3](Vec2 cur, DisplayList& out) {
                previewEntity(*c, makeEntity(solidOf(outline(p1, p2, p3, cur))), out);
            };
            auto fourth = co_await ctx.getPoint("Specify fourth point or <exit>", o4);
            ctx.checkpoint();
            if (fourth.status == cmd::Status::None) {
                addEntity(ctx, solidOf(outline(p1, p2, p3, std::nullopt)));
                co_return;
            }
            if (!fourth.ok()) {
                addEntity(ctx, solidOf(outline(p1, p2, p3, std::nullopt)));
                co_return;
            }
            addEntity(ctx, solidOf(outline(p1, p2, p3, fourth.point)));
            p1 = p3;
            p2 = fourth.point;
        }
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSolid() {
    return std::make_unique<SolidCommand>();
}

}  // namespace tbcad::draw2::detail
