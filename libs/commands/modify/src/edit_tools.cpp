// FIND (find and replace text), RENAME (named rows) and COPYTOLAYER.
#include <algorithm>

#include "commands.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

// ---- FIND ---------------------------------------------------------------------------------------

class FindCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "FIND"; }
    std::string_view description() const override {
        return "Find text in the drawing and replace it (text, mtext, notes, dimensions, "
               "attributes, tables)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("FIND");
        auto what = co_await ctx.getString("Find what");
        if (!what.ok() || what.text.empty())
            co_return;
        auto with = co_await ctx.getString("Replace with (Enter to only count)",
                                           cmd::StringOptions{.allowEmpty = true});
        if (!with.ok())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto cs = co_await ctx.getKeyword("Match case", ko);
        if (!cs.ok())
            co_return;
        bool matchCase = cs.keyword == "Yes";
        auto ww = co_await ctx.getKeyword("Whole words only", ko);
        if (!ww.ok())
            co_return;
        const bool wholeWord = ww.keyword == "Yes";
        bool replace = !with.text.empty();
        // The whole drawing: model space, the objects drawn on every layout sheet and the
        // definitions of the blocks (text inside a block changes in every insert). Generated
        // blocks (dimension graphics, anonymous groups) and xrefs are left alone.
        std::vector<EntityId> ids = ctx.doc().modelSpace();
        std::size_t modelAndSheets = 0;
        for (const auto& [bid, def] : ctx.doc().blocks()) {
            if (def.name.rfind("*LAYOUT-", 0) == 0)
                ids.insert(ids.end(), def.entities.begin(), def.entities.end());
        }
        modelAndSheets = ids.size();
        for (const auto& [bid, def] : ctx.doc().blocks()) {
            if (def.name.empty() || def.name[0] == '*' || !def.xrefPath.empty())
                continue;
            ids.insert(ids.end(), def.entities.begin(), def.entities.end());
        }
        std::size_t objects = 0;
        int matches = 0;
        std::vector<EntityId> found;
        BBox2 foundBox;
        for (std::size_t at = 0; at < ids.size(); ++at) {
            const EntityId id = ids[at];
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            auto r = modify::replaceText(*e, what.text, with.text, matchCase, wholeWord);
            if (!r)
                continue;
            ++objects;
            matches += r->count;
            if (at < modelAndSheets) {  // only what is on a sheet can be shown
                found.push_back(id);
                foundBox.merge(geom::bounds(*e, ctx.doc()));
            }
            if (replace && isModifiable(ctx.doc(), *e))
                (void)ctx.tx().modify(id, [&](Entity& x) { x = std::move(r->entity); });
        }
        if (!found.empty() && !replace) {
            // Show where the matches are: select them and bring them into view.
            ctx.host().setSelection(found);
            cmd::ViewState v = ctx.host().view();
            if (!foundBox.empty() && v.sizePx.x > 0 && v.sizePx.y > 0) {
                v.center = foundBox.center();
                const double needX = foundBox.width() * 1.2 / v.sizePx.x;
                const double needY = foundBox.height() * 1.2 / v.sizePx.y;
                v.scale = std::max({v.scale, needX, needY});
                ctx.host().setView(v);
            }
        }
        if (objects == 0)
            ctx.message("No match for \"" + what.text + "\".");
        else
            ctx.message(std::to_string(matches) + " match(es) in " + std::to_string(objects) +
                        " object(s)" + (replace ? ", replaced." : "."));
    }
};

// ---- COPYTOLAYER --------------------------------------------------------------------------------

class CopyToLayerCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "COPYTOLAYER"; }
    std::string_view description() const override {
        return "Copy objects onto the layer of another object";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("COPYTOLAYER");
        auto sel = co_await ctx.getSelection("Select objects to copy");
        if (!sel.ok())
            co_return;
        auto dest = co_await ctx.getEntity("Select an object on the destination layer");
        if (!dest.ok())
            co_return;
        const Entity* d = ctx.doc().find(dest.id);
        if (!d)
            co_return;
        LayerId layer = d->common.layer;
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            Entity copy = *e;
            copy.common.layer = layer;
            if (ctx.tx().add(std::move(copy)).ok())
                ++n;
        }
        ctx.message(std::to_string(n) + " object(s) copied.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeFind() {
    return std::make_unique<FindCommand>();
}
std::unique_ptr<cmd::Command> makeCopyToLayer() {
    return std::make_unique<CopyToLayerCommand>();
}

}  // namespace tbcad::modifycmd
