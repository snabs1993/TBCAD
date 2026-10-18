// SELECTALL (Ctrl+A): selects every object that can be picked in the drawing.
#include <algorithm>

#include "common.h"

namespace tbcad::select::detail {

namespace {

class SelectAll final : public cmd::Command {
public:
    std::string_view name() const override { return "SELECTALL"; }
    std::string_view description() const override {
        return "Select every object that is shown and not locked";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const Document& doc = ctx.doc();
        std::vector<EntityId> ids;
        doc.forEachEntity([&](const Entity& e) {
            if (!e.common.visible || doc.ownerBlock(e.common.id) != ctx.host().drawingOwner())
                return;
            const EffectiveLayer l = doc.effective(e.common.layer);
            if (l.on && !l.frozen && !l.locked)
                ids.push_back(e.common.id);
        });
        std::sort(ids.begin(), ids.end());
        ctx.host().setSelection(ids);
        ctx.message(std::to_string(ids.size()) + " objects selected.");
        co_return;
    }
};

// SELECT: picks objects and leaves them selected, so the next command works on them at once.
class SelectCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SELECT"; }
    std::string_view description() const override {
        return "Select objects and keep them selected for the next command";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects",
                                             cmd::SelectionOptions{.allowPreselection = false});
        if (sel.status == cmd::Status::Cancel)
            co_return;
        ctx.host().setSelection(sel.ids);
        ctx.message(std::to_string(sel.ids.size()) + " objects selected.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSelectCommand() {
    return std::make_unique<SelectCommand>();
}

std::unique_ptr<cmd::Command> makeSelectAll() {
    return std::make_unique<SelectAll>();
}

}  // namespace tbcad::select::detail
