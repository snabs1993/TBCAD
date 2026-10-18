#include <limits>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"

namespace tbcad::block_cmds {

namespace {

constexpr int kMaxNesting = 8;

// The part of a block or xref instance closest to `at`, in the space of the drawing: the insert is
// broken up one level at a time while the closest part is itself an insert.
std::optional<Entity> nestedAt(const Document& doc, const Entity& insert, Vec2 at) {
    Entity current = insert;
    for (int depth = 0; depth < kMaxNesting; ++depth) {
        auto parts = blocks::explode(doc, current);
        if (!parts || parts.value().entities.empty())
            return std::nullopt;
        const Entity* best = nullptr;
        double bestDistance = std::numeric_limits<double>::infinity();
        for (const Entity& part : parts.value().entities) {
            const double d = geom::distance(part, at, &doc);
            if (d < bestDistance) {
                bestDistance = d;
                best = &part;
            }
        }
        if (!best)
            return std::nullopt;
        if (kind(*best) != EntityKind::Insert)
            return *best;
        current = *best;
    }
    return std::nullopt;
}

// NCOPY: copies one object that lives inside a block or an external reference into the drawing
// itself, at its place in the drawing, then moves the copy.
class NCopyCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "NCOPY"; }
    std::vector<std::string> defaultAliases() const override { return {}; }
    std::string_view description() const override {
        return "Copy an object out of a block or external reference";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::EntityOptions eo;
        eo.filter = [](const Entity& e) { return kind(e) == EntityKind::Insert; };
        auto picked = co_await ctx.getEntity("Select nested object to copy", eo);
        if (!picked.ok())
            co_return;
        const Entity* insert = doc.find(picked.id);
        if (!insert)
            co_return;
        auto part = nestedAt(doc, *insert, picked.pickPoint);
        if (!part) {
            ctx.message("There is no object inside that block to copy.");
            co_return;
        }
        const Entity source = *part;

        cmd::PointOptions bo;
        bo.allowNone = true;
        auto base = co_await ctx.getPoint("Specify base point <no move>", bo);
        Vec2 shift{};
        if (base.ok()) {
            cmd::PointOptions po;
            po.base = base.point;
            po.preview = [&doc, &ctx, source, from = base.point](Vec2 to, DisplayList& out) {
                auto moved = geom::transformed(source, Transform2::translate(to - from));
                if (!moved)
                    return;
                geom::DisplayContext dc;
                dc.pixelSizeMm = ctx.host().view().scale;
                geom::buildDisplay(doc, moved.value(), dc, out);
            };
            auto to = co_await ctx.getPoint("Specify second point <use first point as shift>", po);
            if (to.status == cmd::Status::Cancel)
                co_return;
            if (to.ok())
                shift = to.point - base.point;
        } else if (base.status == cmd::Status::Cancel) {
            co_return;
        }

        auto moved = geom::transformed(source, Transform2::translate(shift));
        if (!moved) {
            ctx.message(moved.error().message);
            co_return;
        }
        auto added = ctx.tx().add(std::move(moved).value());
        if (!added) {
            ctx.message(added.error().message);
            co_return;
        }
        ctx.host().setSelection({added.value()});
        ctx.setUndoLabel("NCOPY");
        ctx.message("Copied 1 object out of the block");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeNCopyCommand() {
    return std::make_unique<NCopyCommand>();
}

}  // namespace tbcad::block_cmds
