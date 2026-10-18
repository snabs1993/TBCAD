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

}  // namespace

std::unique_ptr<cmd::Command> makeSelectAll() {
    return std::make_unique<SelectAll>();
}

}  // namespace tbcad::select::detail
