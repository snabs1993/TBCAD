#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/underlay/pdf.h"

namespace tbcad::block_cmds {

namespace {

// XCLIP [XC]: limits an inserted block or xref to a boundary. The boundary is stored in the
// block's coordinates, so it moves, turns and scales with the insert.
class XClipCommand final : public cmd::Command {
public:
    // `pdf`: PDFCLIP, the same for PDF underlays only.
    explicit XClipCommand(bool pdf = false) : pdf_(pdf) {}
    std::string_view name() const override { return pdf_ ? "PDFCLIP" : "XCLIP"; }
    std::vector<std::string> defaultAliases() const override {
        return pdf_ ? std::vector<std::string>{} : std::vector<std::string>{"XC"};
    }
    std::string_view description() const override {
        return pdf_ ? "Clip a PDF underlay to a rectangle or polygon"
                    : "Clip a block or external reference to a boundary";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::EntityOptions eo;
        eo.filter = [&doc, this](const Entity& e) {
            const auto* ins = entityAs<InsertData>(e);
            const BlockDef* def = ins ? doc.block(ins->block) : nullptr;
            return def && !ins->array && (!pdf_ || underlay::isPdfPath(def->xrefPath));
        };
        auto picked = co_await ctx.getEntity(
            pdf_ ? "Select a PDF underlay to clip" : "Select a block or xref to clip", eo);
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

private:
    bool pdf_;
};

// XCLIPFRAME and PDFFRAME: 0 hides the clip boundary, 1 shows and plots it, 2 shows it on screen
// only. XCLIPFRAME is for blocks and xrefs, PDFFRAME for PDF underlays. Settings of the drawing.
class ClipFrameCommand final : public cmd::Command {
public:
    explicit ClipFrameCommand(bool pdf) : pdf_(pdf) {}
    std::string_view name() const override { return pdf_ ? "PDFFRAME" : "XCLIPFRAME"; }
    std::string_view description() const override {
        return pdf_ ? "Show, hide or plot the clip boundary of PDF underlays"
                    : "Show, hide or plot the clip boundary of blocks and xrefs";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const char* key = pdf_ ? kPdfFrameKey : kXclipFrameKey;
        const int now = clipFrameMode(ctx.doc().header(), key);
        cmd::IntOptions io;
        io.defaultValue = now;
        io.min = 0;
        io.max = 2;
        auto v = co_await ctx.getInteger(std::string(name()) +
                                             " 0 hidden, 1 shown and plotted, 2 shown not "
                                             "plotted <" +
                                             std::to_string(now) + ">",
                                         io);
        if (!v.ok())
            co_return;
        const std::string value = std::to_string(std::clamp<long long>(v.value, 0, 2));
        (void)ctx.tx().modifyHeader([&](Header& h) { h.custom[key] = value; });
        ctx.setUndoLabel(std::string(name()));
        ctx.message(std::string(name()) + " = " + value);
    }

private:
    bool pdf_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeClipFrameCommand(bool pdf) {
    return std::make_unique<ClipFrameCommand>(pdf);
}

std::unique_ptr<cmd::Command> makePdfClipCommand() {
    return std::make_unique<XClipCommand>(true);
}

std::unique_ptr<cmd::Command> makeXClipCommand() {
    return std::make_unique<XClipCommand>();
}

}  // namespace tbcad::block_cmds
