#include <unordered_set>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

// BLOCK [B]: turns the selected objects into a named block definition.
class BlockCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BLOCK"; }
    std::vector<std::string> defaultAliases() const override { return {"B"}; }
    std::string_view description() const override { return "Make a block from objects"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();

        // 1. Name. An existing name may be redefined.
        std::string blockName;
        BlockId existing{};
        for (;;) {
            auto n = co_await ctx.getString("Enter block name");
            if (!n.ok())
                co_return;
            blockName = cmd::trim(n.text);
            if (const BlockDef* def = doc.blockByName(blockName)) {
                auto yn = co_await ctx.getKeyword(
                    "Block \"" + def->name + "\" already exists. Redefine it? [Yes/No] <No>",
                    cmd::KeywordOptions{.keywords = {"Yes", "No"}, .defaultKeyword = "No"});
                if (!yn.ok())
                    co_return;
                if (yn.keyword == "Yes") {
                    existing = def->id;
                    blockName = def->name;
                    break;
                }
                continue;
            }
            auto ok = blocks::validateBlockName(doc, blockName);
            if (ok.ok())
                break;
            ctx.message(ok.error().message);
        }

        // 2. Base point.
        auto base = co_await ctx.getPoint("Specify base point <0,0>");
        if (base.status == cmd::Status::Cancel)
            co_return;
        const Vec2 basePoint = base.ok() ? base.point : Vec2{};

        // 3. Objects.
        auto sel = co_await ctx.getSelection(
            "Select objects", cmd::SelectionOptions{.filter = detail::editableFilter(doc)});
        if (!sel.ok() || sel.ids.empty())
            co_return;
        const std::vector<EntityId> ids = detail::inDrawOrder(doc, sel.ids);
        if (ids.empty()) {
            ctx.message("Nothing selectable to put in a block");
            co_return;
        }

        // 4. Cycle check: a block cannot end up containing itself.
        if (existing) {
            for (EntityId id : ids)
                if (const auto* ins = entityAs<InsertData>(*doc.find(id)))
                    if (blocks::wouldCreateCycle(doc, existing, ins->block)) {
                        ctx.message("Block \"" + blockName +
                                    "\" cannot contain itself, directly or through another block");
                        co_return;
                    }
        }

        // Attribute tags must be unique inside one block.
        {
            std::vector<const Entity*> defs;
            for (EntityId id : ids)
                if (const auto* t = entityAs<TextData>(*doc.find(id)); t && t->attdef)
                    defs.push_back(doc.find(id));
            std::string dup = blocks::duplicateTag(defs);
            if (!dup.empty()) {
                ctx.message("Two attributes in the block have the tag " + dup +
                            ". Give each attribute its own tag.");
                co_return;
            }
        }

        // 5. What happens to the originals.
        auto what =
            co_await ctx.getKeyword("Original objects [Retain/Convert/Delete] <Convert>",
                                    cmd::KeywordOptions{.keywords = {"Retain", "Convert", "Delete"},
                                                        .defaultKeyword = "Convert"});
        if (!what.ok())
            co_return;

        // Copy first: erasing the originals changes the document.
        std::vector<Entity> content;
        content.reserve(ids.size());
        for (EntityId id : ids)
            content.push_back(*doc.find(id));

        Transaction& tx = ctx.tx();
        BlockId block = existing;
        if (existing) {
            std::vector<EntityId> old = doc.block(existing)->entities;
            for (EntityId id : old)
                (void)tx.erase(id);
            (void)tx.modifyBlock(existing, [&](BlockDef& d) { d.base = basePoint; });
        } else {
            auto added = tx.addBlock(BlockDef{.name = blockName, .base = basePoint});
            if (!added) {
                ctx.message(added.error().message);
                co_return;
            }
            block = added.value();
        }
        for (Entity& e : content) {
            e.common.id = {};
            auto r = tx.add(std::move(e), block);
            if (!r) {
                ctx.message(r.error().message);
                co_return;  // the transaction is rolled back
            }
        }

        std::vector<EntityId> selection;
        if (what.keyword != "Retain") {
            for (EntityId id : ids)
                (void)tx.erase(id);
        }
        if (existing) {
            std::size_t synced = blocks::syncInserts(tx, doc, block);
            if (synced > 0)
                ctx.message("Attributes of " + std::to_string(synced) +
                            " existing inserts updated");
        }
        if (what.keyword == "Convert") {
            InsertData data{block, basePoint, {1, 1}, 0};
            data.attribs = blocks::defaultAttribs(doc, block);
            Entity ins;
            ins.data = Indirect<InsertData>(std::move(data));
            auto r = tx.add(std::move(ins));
            if (r)
                selection.push_back(r.value());
        }
        ctx.host().setSelection(selection);
        ctx.setUndoLabel("Block " + blockName);
        ctx.message((existing ? "Block \"" + blockName + "\" redefined with "
                              : "Block \"" + blockName + "\" defined with ") +
                    detail::objects(ids.size()));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBlockCommand() {
    return std::make_unique<BlockCommand>();
}

}  // namespace tbcad::block_cmds
