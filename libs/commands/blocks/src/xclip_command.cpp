#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

// XCLIP [XC]: limits an inserted block or xref to a boundary. The boundary is stored in the
// block's coordinates, so it moves, turns and scales with the insert.
class XClipCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "XCLIP"; }
    std::vector<std::string> defaultAliases() const override { return {"XC"}; }
    std::string_view description() const override {
        return "Clip a block or external reference to a boundary";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::EntityOptions eo;
        eo.filter = [&doc](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            return ins && doc.block(ins->block) && !ins->array;
        };
        auto picked = co_await ctx.getEntity("Select a block or xref to clip", eo);
        if (!picked.ok())
            co_return;
        const EntityId id = picked.id;
        const Entity* e = doc.find(id);
        const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
        if (!ins)
            co_return;
        const bool has = ins->clip.size() >= 3;
        auto opt = co_await ctx.getKeyword(
            std::string("Clip option [ON/OFF/Delete/New/Invert] <") + (has ? "ON" : "New") + ">",
            cmd::KeywordOptions{.keywords = {"ON", "OFF", "Delete", "New", "Invert"},
                                .defaultKeyword = has ? "ON" : "New"});
        if (!opt.ok())
            co_return;

        if (opt.keyword == "Invert") {
            if (!has) {
                ctx.message("This object has no clipping boundary");
                co_return;
            }
            const bool now = !ins->clipInvert;
            (void)ctx.tx().modify(id, [&](Entity& en) {
                auto* d = entityAs<InsertData>(en);
                d->clipInvert = now;
                d->clipOn = true;
            });
            ctx.setUndoLabel("Invert clip");
            ctx.message(now ? "Clipping inverted: the inside is hidden"
                            : "Clipping restored: the outside is hidden");
            co_return;
        }
        if (opt.keyword == "ON" || opt.keyword == "OFF" || opt.keyword == "Delete") {
            if (!has) {
                ctx.message("This object has no clipping boundary");
                co_return;
            }
            const bool on = opt.keyword == "ON";
            const bool del = opt.keyword == "Delete";
            (void)ctx.tx().modify(id, [&](Entity& en) {
                auto* d = entityAs<InsertData>(en);
                if (del) {
                    d->clip.clear();
                    d->clipInvert = false;
                }
                d->clipOn = del ? true : on;
            });
            ctx.setUndoLabel(del ? "Delete clip" : on ? "Clip on" : "Clip off");
            ctx.message(del  ? "Clipping boundary deleted"
                        : on ? "Clipping is on"
                             : "Clipping is off");
            co_return;
        }

        auto shape =
            co_await ctx.getKeyword("Boundary [Rectangular/Polygonal] <Rectangular>",
                                    cmd::KeywordOptions{.keywords = {"Rectangular", "Polygonal"},
                                                        .defaultKeyword = "Rectangular"});
        if (!shape.ok())
            co_return;
        std::vector<Vec2> world;
        if (shape.keyword == "Rectangular") {
            auto a = co_await ctx.getPoint("Specify first corner");
            if (!a.ok())
                co_return;
            auto b = co_await ctx.getPoint("Specify opposite corner",
                                           cmd::PointOptions{.base = a.point});
            if (!b.ok())
                co_return;
            if (std::abs(a.point.x - b.point.x) < 1e-9 || std::abs(a.point.y - b.point.y) < 1e-9) {
                ctx.message("The corners must differ in both directions");
                co_return;
            }
            world = {a.point, {b.point.x, a.point.y}, b.point, {a.point.x, b.point.y}};
        } else {
            for (;;) {
                const std::string prompt = world.size() < 3
                                               ? "Specify point " + std::to_string(world.size() + 1)
                                               : "Specify next point or Enter to close";
                auto p = co_await ctx.getPoint(
                    prompt,
                    cmd::PointOptions{
                        .base = world.empty() ? std::nullopt : std::optional<Vec2>(world.back()),
                        .allowNone = world.size() >= 3});
                if (p.status == cmd::Status::None)
                    break;
                if (!p.ok())
                    co_return;
                world.push_back(p.point);
            }
        }
        const BlockDef* def = doc.block(ins->block);
        auto inv = blocks::instanceTransform(*ins, def->base).inverse();
        if (!inv) {
            ctx.message("The block is scaled to nothing; it cannot be clipped");
            co_return;
        }
        std::vector<Vec2> local;
        local.reserve(world.size());
        for (const Vec2& p : world)
            local.push_back(inv.value().apply(p));
        (void)ctx.tx().modify(id, [&](Entity& en) {
            auto* d = entityAs<InsertData>(en);
            d->clip = std::move(local);
            d->clipOn = true;
            d->clipInvert = false;
        });
        ctx.setUndoLabel("Clip");
        ctx.message("Clipped to a boundary of " + std::to_string(world.size()) + " points");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeXClipCommand() {
    return std::make_unique<XClipCommand>();
}

}  // namespace tbcad::block_cmds
