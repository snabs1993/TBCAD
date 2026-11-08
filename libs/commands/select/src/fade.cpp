#include <algorithm>

#include "common.h"

namespace tbcad::select::detail {
namespace {

// FADE: the picked objects are drawn and plotted in a light grey. Only the picked objects change:
// fading one reference of a block leaves its other references as they are. Picking objects that
// are all faded already brings their own colours back.
class Fade final : public cmd::Command {
public:
    std::string_view name() const override { return "FADE"; }
    std::string_view description() const override {
        return "Fade the selected objects to a light grey, or bring their colours back";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects to fade or unfade");
        if (!sel.ok() || sel.ids.empty())
            co_return;
        const Document& doc = ctx.doc();
        const bool allFaded = std::all_of(sel.ids.begin(), sel.ids.end(), [&](EntityId id) {
            const Entity* e = doc.find(id);
            return !e || e->common.faded;
        });
        const bool fade = !allFaded;
        int n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = doc.find(id);
            if (!e || e->common.faded == fade)
                continue;
            if (ctx.tx().modify(id, [fade](Entity& x) { x.common.faded = fade; }))
                ++n;
        }
        ctx.setUndoLabel(fade ? "FADE" : "UNFADE");
        ctx.message(std::to_string(n) +
                    (fade ? " object(s) faded. FADE them again to bring their colours back."
                          : " object(s) shown in their own colours again."));
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeFade() {
    return std::make_unique<Fade>();
}

}  // namespace tbcad::select::detail
