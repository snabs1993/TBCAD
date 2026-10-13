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
        auto with = co_await ctx.getString("Replace with (Enter to only count)");
        if (!with.ok())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Yes", "No"};
        ko.defaultKeyword = "No";
        auto cs = co_await ctx.getKeyword("Match case", ko);
        if (!cs.ok())
            co_return;
        bool matchCase = cs.keyword == "Yes";
        bool replace = !with.text.empty();
        std::size_t objects = 0;
        int matches = 0;
        std::vector<EntityId> ids = ctx.doc().modelSpace();
        for (EntityId id : ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            auto r = modify::replaceText(*e, what.text, with.text, matchCase);
            if (!r)
                continue;
            ++objects;
            matches += r->count;
            if (replace && isModifiable(ctx.doc(), *e))
                (void)ctx.tx().modify(id, [&](Entity& x) { x = std::move(r->entity); });
        }
        if (objects == 0)
            ctx.message("No match for \"" + what.text + "\".");
        else
            ctx.message(std::to_string(matches) + " match(es) in " + std::to_string(objects) +
                        " object(s)" + (replace ? ", replaced." : "."));
    }
};

// ---- RENAME -------------------------------------------------------------------------------------

class RenameCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "RENAME"; }
    std::vector<std::string> defaultAliases() const override { return {"REN"}; }
    std::string_view description() const override {
        return "Rename a layer, block, text style or dimension style";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("RENAME");
        cmd::KeywordOptions ko;
        ko.keywords = {"Layer", "Block", "TextStyle", "DimStyle"};
        auto kind = co_await ctx.getKeyword("Rename [Layer/Block/TextStyle/DimStyle]", ko);
        if (!kind.ok())
            co_return;
        auto from = co_await ctx.getString("Old name");
        if (!from.ok() || from.text.empty())
            co_return;
        auto to = co_await ctx.getString("New name");
        if (!to.ok() || to.text.empty())
            co_return;
        const Document& doc = ctx.doc();
        Result<void> r =
            Error{ErrorCode::NotFound, "No " + kind.keyword + " named \"" + from.text + "\"."};
        const std::string& n = to.text;
        if (kind.keyword == "Layer") {
            if (const Layer* l = doc.layerByName(from.text)) {
                if (l->name == "0" && !l->parent)
                    r = Error{ErrorCode::Conflict, "Layer 0 cannot be renamed."};
                else if (doc.layerByName(l->parent, n))
                    r = Error{ErrorCode::Conflict, "A layer named \"" + n + "\" already exists."};
                else
                    r = ctx.tx().modifyLayer(l->id, [&](Layer& x) { x.name = n; });
            }
        } else if (kind.keyword == "Block") {
            if (const BlockDef* b = doc.blockByName(from.text)) {
                if (doc.blockByName(n))
                    r = Error{ErrorCode::Conflict, "A block named \"" + n + "\" already exists."};
                else
                    r = ctx.tx().modifyBlock(b->id, [&](BlockDef& x) { x.name = n; });
            }
        } else if (kind.keyword == "TextStyle") {
            if (const TextStyle* s = doc.textStyleByName(from.text)) {
                if (doc.textStyleByName(n))
                    r = Error{ErrorCode::Conflict, "A text style named \"" + n + "\" exists."};
                else
                    r = ctx.tx().modifyTextStyle(s->id, [&](TextStyle& x) { x.name = n; });
            }
        } else {
            if (const DimStyle* s = doc.dimStyleByName(from.text)) {
                if (doc.dimStyleByName(n))
                    r = Error{ErrorCode::Conflict, "A dimension style named \"" + n + "\" exists."};
                else
                    r = ctx.tx().modifyDimStyle(s->id, [&](DimStyle& x) { x.name = n; });
            }
        }
        ctx.message(r ? "Renamed " + from.text + " to " + n + "." : r.error().message);
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
std::unique_ptr<cmd::Command> makeRename() {
    return std::make_unique<RenameCommand>();
}
std::unique_ptr<cmd::Command> makeCopyToLayer() {
    return std::make_unique<CopyToLayerCommand>();
}

}  // namespace tbcad::modifycmd
