#include <algorithm>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

namespace tbcad::block_cmds {

namespace {

// EXPLODE [X]: replaces inserts and polylines with their parts, one level at a time.
class ExplodeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "EXPLODE"; }
    std::vector<std::string> defaultAliases() const override { return {"X"}; }
    std::string_view description() const override {
        return "Break blocks and polylines into parts";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        auto base = detail::editableFilter(doc);
        auto sel = co_await ctx.getSelection(
            "Select objects to explode", cmd::SelectionOptions{.filter = [base](const Entity& e) {
                return base(e) && blocks::canExplode(kind(e));
            }});
        if (!sel.ok())
            co_return;
        if (sel.ids.empty()) {
            ctx.message("0 found. Nothing to explode was selected.");
            co_return;
        }

        std::vector<EntityId> created;
        std::size_t exploded = 0;
        std::vector<std::string> notes;
        const std::vector<EntityId> ids = detail::inDrawOrder(doc, sel.ids);
        Transaction& tx = ctx.tx();
        for (EntityId id : ids) {
            const Entity* e = doc.find(id);
            if (!e || !blocks::canExplode(kind(*e)))
                continue;
            auto r = blocks::explode(doc, *e);
            if (!r) {
                ctx.message(r.error().message);
                continue;
            }
            for (std::string& m : r.value().messages)
                if (std::find(notes.begin(), notes.end(), m) == notes.end())
                    notes.push_back(std::move(m));
            (void)tx.erase(id);
            for (Entity& part : r.value().entities) {
                auto added = tx.add(std::move(part));
                if (added)
                    created.push_back(added.value());
                else
                    ctx.message(added.error().message);
            }
            ++exploded;
        }
        for (const std::string& m : notes)
            ctx.message(m);
        if (exploded == 0) {
            ctx.message("Nothing could be exploded");
            co_return;
        }
        ctx.host().setSelection(created);
        ctx.message("Exploded " + detail::objects(exploded) + " into " +
                    detail::objects(created.size()));
        ctx.setUndoLabel("Explode " + detail::objects(exploded));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeExplodeCommand() {
    return std::make_unique<ExplodeCommand>();
}

}  // namespace tbcad::block_cmds
