// DIMREASSOC and DIMDISASSOC: link dimensions to the objects they measure, or cut the link.
#include "dim_internal.h"
#include "tbcad/core/dim_assoc.h"

namespace tbcad::dimcmd {

namespace {

bool isDim(const Entity& e) {
    return entityAs<DimensionData>(e) != nullptr;
}

class DimReassocCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMREASSOC"; }
    std::string_view description() const override {
        return "Link dimensions to the objects whose points they measure";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = isDim;
        auto sel = co_await ctx.getSelection("Select dimensions to re-associate", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("DIMREASSOC");
        int dims = 0, points = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !isDim(*e))
                continue;
            DimensionData d = *entityAs<DimensionData>(*e);
            const int n = linkDimension(ctx.doc(), d, ctx.doc().ownerBlock(id));
            if (!ctx.tx().modify(id, [&](Entity& x) { *entityAs<DimensionData>(x) = d; }))
                continue;
            if (n > 0)
                ++dims;
            points += n;
        }
        ctx.message(
            std::to_string(dims) + " dimensions linked at " + std::to_string(points) +
            " points. A point is linked where it sits exactly on an end, vertex or centre.");
    }
};

class DimDisassocCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMDISASSOC"; }
    std::string_view description() const override {
        return "Cut the links between dimensions and the objects they measure";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = isDim;
        auto sel = co_await ctx.getSelection("Select dimensions to disassociate", so);
        if (!sel.ok())
            co_return;
        ctx.setUndoLabel("DIMDISASSOC");
        int n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || !isDim(*e))
                continue;
            bool linked = false;
            for (const DimAssoc& a : entityAs<DimensionData>(*e)->assoc)
                linked = linked || a.kind != DimAssoc::Kind::None;
            if (!linked)
                continue;
            if (ctx.tx().modify(id, [](Entity& x) { entityAs<DimensionData>(x)->assoc = {}; }))
                ++n;
        }
        ctx.message(std::to_string(n) + " dimensions disassociated.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeDimReassocCommand() {
    return std::make_unique<DimReassocCommand>();
}
std::unique_ptr<cmd::Command> makeDimDisassocCommand() {
    return std::make_unique<DimDisassocCommand>();
}

}  // namespace tbcad::dimcmd
