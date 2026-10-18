// BLOCKREPLACE: swaps the block of the selected inserts for another block. Position, scale and
// rotation stay; attribute values are rebuilt for the new block.
#include "commands.h"
#include "tbcad/blocks/attributes.h"

namespace tbcad::block_cmds {

namespace {

class BlockReplaceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BLOCKREPLACE"; }
    std::string_view description() const override {
        return "Replace the block of the selected inserts with another block";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) { return entityAs<InsertData>(e) != nullptr; };
        auto sel = co_await ctx.getSelection("Select block references to replace", so);
        if (!sel.ok())
            co_return;
        auto name = co_await ctx.getString("Name of the replacement block");
        if (!name.ok())
            co_return;
        const BlockDef* nb = ctx.doc().blockByName(cmd::trim(name.text));
        if (!nb) {
            ctx.message("There is no block named \"" + cmd::trim(name.text) + "\".");
            co_return;
        }
        const BlockId target = nb->id;
        ctx.setUndoLabel("BLOCKREPLACE");
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            auto r = ctx.tx().modify(id, [&](Entity& e) {
                if (auto* ins = entityAs<InsertData>(e)) {
                    ins->block = target;
                    ins->attribs.clear();
                    ins->dynValues.clear();
                    ins->asmValues.clear();
                    ins->clip.clear();
                }
            });
            if (r)
                ++n;
        }
        (void)blocks::syncInserts(ctx.tx(), ctx.doc(), target);
        ctx.message(std::to_string(n) + " block reference(s) replaced.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBlockReplaceCommand() {
    return std::make_unique<BlockReplaceCommand>();
}

}  // namespace tbcad::block_cmds
