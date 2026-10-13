// RENAME: layers, blocks, linetypes, text styles and dimension styles under one command.
#include <string>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"

namespace tbcad::sysvars {

namespace {

using cmd::CommandContext;
using cmd::Task;

bool sameName(const std::string& a, const std::string& b) {
    return cmd::toUpperAscii(a) == cmd::toUpperAscii(b);
}

class RenameCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "RENAME"; }
    std::vector<std::string> defaultAliases() const override { return {"REN"}; }
    std::string_view description() const override {
        return "Rename a layer, block, linetype, text style or dimension style";
    }

    Task run(CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Layer", "Block", "Linetype", "Textstyle", "Dimstyle"};
        ko.defaultKeyword = "Layer";
        auto kind = co_await ctx.getKeyword("Rename [Layer/Block/Linetype/Textstyle/Dimstyle]", ko);
        if (!kind.ok())
            co_return;
        cmd::StringOptions so;
        auto from = co_await ctx.getString("Old name", so);
        if (!from.ok())
            co_return;
        auto to = co_await ctx.getString("New name", so);
        if (!to.ok())
            co_return;
        const std::string oldName = cmd::trim(from.text);
        const std::string newName = cmd::trim(to.text);
        if (newName.empty()) {
            ctx.message("The new name is empty.");
            co_return;
        }
        ctx.setUndoLabel("Rename");
        const Document& doc = ctx.doc();
        Result<void> r;
        if (kind.keyword == "Layer") {
            auto id = layers::resolveLayer(doc, oldName);
            if (!id) {
                ctx.message(id.error().message);
                co_return;
            }
            r = layers::renameLayer(doc, ctx.tx(), id.value(), newName);
        } else if (kind.keyword == "Block") {
            const BlockDef* b = doc.blockByName(oldName);
            if (!b || b->anonymous) {
                ctx.message("There is no block named " + oldName + ".");
                co_return;
            }
            if (!b->xrefPath.empty()) {
                ctx.message(
                    "An external reference is renamed by detaching and attaching it again.");
                co_return;
            }
            if (const BlockDef* o = doc.blockByName(newName); o && o->id != b->id) {
                ctx.message("A block named " + newName + " already exists.");
                co_return;
            }
            r = ctx.tx().modifyBlock(b->id, [&](BlockDef& row) { row.name = newName; });
        } else if (kind.keyword == "Linetype") {
            const Linetype* l = doc.linetypeByName(oldName);
            if (!l) {
                ctx.message("There is no linetype named " + oldName + ".");
                co_return;
            }
            if (l->pattern.empty() && sameName(l->name, "Continuous")) {
                ctx.message("The Continuous linetype cannot be renamed.");
                co_return;
            }
            if (const Linetype* o = doc.linetypeByName(newName); o && o->id != l->id) {
                ctx.message("A linetype named " + newName + " already exists.");
                co_return;
            }
            r = ctx.tx().modifyLinetype(l->id, [&](Linetype& row) { row.name = newName; });
        } else if (kind.keyword == "Textstyle") {
            const TextStyle* s = doc.textStyleByName(oldName);
            if (!s) {
                ctx.message("There is no text style named " + oldName + ".");
                co_return;
            }
            if (s->id == kTextStyleStandard) {
                ctx.message("The Standard style cannot be renamed.");
                co_return;
            }
            if (const TextStyle* o = doc.textStyleByName(newName); o && o->id != s->id) {
                ctx.message("A text style named " + newName + " already exists.");
                co_return;
            }
            r = ctx.tx().modifyTextStyle(s->id, [&](TextStyle& row) { row.name = newName; });
        } else {
            const DimStyle* s = doc.dimStyleByName(oldName);
            if (!s) {
                ctx.message("There is no dimension style named " + oldName + ".");
                co_return;
            }
            if (s->id == kDimStyleStandard) {
                ctx.message("The Standard style cannot be renamed.");
                co_return;
            }
            if (const DimStyle* o = doc.dimStyleByName(newName); o && o->id != s->id) {
                ctx.message("A dimension style named " + newName + " already exists.");
                co_return;
            }
            r = ctx.tx().modifyDimStyle(s->id, [&](DimStyle& row) { row.name = newName; });
        }
        if (!r)
            ctx.message(r.error().message);
        else
            ctx.message("Renamed " + oldName + " to " + newName + ".");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeRenameCommand() {
    return std::make_unique<RenameCommand>();
}

}  // namespace tbcad::sysvars
