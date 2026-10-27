#include <cstdio>
#include <unordered_set>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/xref/xref.h"

namespace tbcad::block_cmds {

namespace {

// BLOCK [B]: turns the selected objects into a named block definition. ATTREDEF is the same
// for a block that exists: it is redefined from the objects (attribute definitions included),
// the inserts keep their attribute values by tag, and the objects are used up.
// In the window BLOCK opens the Make Block dialog (name, objects, base point, what happens to the
// originals); its buttons run BLOCK again with the answers in "tmp.block.*" settings. -BLOCK (and
// any host without a window) asks everything on the command line.
class BlockCommand final : public cmd::Command {
public:
    explicit BlockCommand(bool redefine = false, bool lineOnly = false)
        : redefine_(redefine), lineOnly_(lineOnly || redefine) {}
    std::string_view name() const override {
        return redefine_ ? "ATTREDEF" : lineOnly_ ? "-BLOCK" : "BLOCK";
    }
    std::vector<std::string> defaultAliases() const override {
        return redefine_ || lineOnly_ ? std::vector<std::string>{} : std::vector<std::string>{"B"};
    }
    std::string_view description() const override {
        return redefine_ ? "Redefine a block and update its attributes"
                         : "Make a block from objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        cmd::IHost& host = ctx.host();
        const bool dialog = !lineOnly_ && host.dialogsAvailable();
        auto setting = [&host](const char* key) { return host.setting(key).value_or(""); };
        if (dialog) {
            const std::string pick = setting("tmp.block.pick");
            host.setSetting("tmp.block.pick", "");
            if (pick == "base") {
                auto p = co_await ctx.getPoint("Specify base point");
                if (p.ok())
                    host.setSetting("tmp.block.base", blockPointText(p.point));
                host.openPanel("makeblock");
                co_return;
            }
            if (pick == "objects") {
                auto sel = co_await ctx.getSelection(
                    "Select objects", cmd::SelectionOptions{.filter = detail::editableFilter(doc)});
                if (sel.status != cmd::Status::Cancel && !sel.ids.empty())
                    host.setSelection(sel.ids);
                host.openPanel("makeblock");
                co_return;
            }
            if (setting("tmp.block.go") != "1") {
                host.openPanel("makeblock");
                ctx.message(
                    "Use the Make Block panel (name, objects, base point), then press OK. Type "
                    "-BLOCK to answer on the command line instead.");
                co_return;
            }
            host.setSetting("tmp.block.go", "0");
        }

        // 1. Name. An existing name may be redefined.
        std::string blockName;
        BlockId existing{};
        if (dialog) {
            blockName = cmd::trim(setting("tmp.block.name"));
            if (const BlockDef* def = doc.blockByName(blockName)) {
                existing = def->id;
                blockName = def->name;
            } else if (auto ok = blocks::validateBlockName(doc, blockName); !ok.ok()) {
                ctx.message(ok.error().message);
                co_return;
            }
        }
        for (; !dialog;) {
            auto n = co_await ctx.getString(redefine_ ? "Enter name of the block to redefine"
                                                      : "Enter block name");
            if (!n.ok())
                co_return;
            blockName = cmd::trim(n.text);
            if (redefine_) {
                const BlockDef* def = doc.blockByName(blockName);
                if (!def) {
                    ctx.message("No block named \"" + blockName + "\".");
                    continue;
                }
                existing = def->id;
                blockName = def->name;
                break;
            }
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

        // 2. Objects.
        cmd::SelectionResult sel;
        if (dialog) {
            for (EntityId id : host.selection())
                if (const Entity* e = doc.find(id); e && detail::editableFilter(doc)(*e))
                    sel.ids.push_back(id);
            sel.status = cmd::Status::Ok;
        } else {
            sel = co_await ctx.getSelection(
                "Select objects", cmd::SelectionOptions{.filter = detail::editableFilter(doc)});
        }
        if (sel.status == cmd::Status::Cancel)
            co_return;
        if (sel.ids.empty()) {
            ctx.message("No objects selected. The block was not made.");
            co_return;
        }
        const std::vector<EntityId> ids = detail::inDrawOrder(doc, sel.ids);
        if (ids.empty()) {
            ctx.message("Nothing selectable to put in a block");
            co_return;
        }

        // 3. Base point: Enter takes the lower-left corner of the objects, so the insert lands
        // where it is clicked instead of at the drawing's origin.
        Vec2 basePoint{};
        {
            std::vector<Entity> picked;
            for (EntityId id : ids)
                picked.push_back(*doc.find(id));
            const BBox2 box = detail::boundsOf(doc, picked);
            const Vec2 lowerLeft = box.empty() ? Vec2{} : box.min;
            auto fmt = [](double v) {
                char buf[48];
                std::snprintf(buf, sizeof buf, "%.2f", v);
                std::string t = buf;
                while (t.find('.') != std::string::npos && (t.back() == '0' || t.back() == '.')) {
                    const bool dot = t.back() == '.';
                    t.pop_back();
                    if (dot)
                        break;
                }
                return t == "-0" ? std::string("0") : t;
            };
            if (dialog) {
                basePoint = lowerLeft;  // the dialog's base point, when it has one
                const std::string bx = setting("tmp.block.base");
                if (const auto comma = bx.find(','); comma != std::string::npos)
                    basePoint = {std::strtod(bx.c_str(), nullptr),
                                 std::strtod(bx.c_str() + comma + 1, nullptr)};
            } else {
                auto base =
                    co_await ctx.getPoint("Specify base point <" + fmt(lowerLeft.x) + "," +
                                          fmt(lowerLeft.y) + ", the lower-left of the objects>");
                if (base.status == cmd::Status::Cancel)
                    co_return;
                basePoint = base.ok() ? base.point : lowerLeft;
            }
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
        cmd::KeywordResult what{cmd::Status::Keyword, "Delete"};
        if (dialog) {
            const std::string m = setting("tmp.block.mode");
            what.keyword = m == "Retain" || m == "Delete" ? m : "Convert";
        } else if (!redefine_) {
            what = co_await ctx.getKeyword(
                "Original objects [Retain/Convert/Delete] <Convert>",
                cmd::KeywordOptions{.keywords = {"Retain", "Convert", "Delete"},
                                    .defaultKeyword = "Convert"});
            if (!what.ok())
                co_return;
        }

        // Copy first: erasing the originals changes the document.
        std::vector<Entity> content;
        content.reserve(ids.size());
        for (EntityId id : ids)
            content.push_back(*doc.find(id));

        Transaction& tx = ctx.tx();
        BlockId block = existing;
        if (existing) {
            (void)tx.eraseAllIn(existing);
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

private:
    static std::string blockPointText(Vec2 p) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.10g,%.10g", p.x, p.y);
        return buf;
    }

    bool redefine_;
    bool lineOnly_;
};

// BASE: the point of this drawing that lands on the insertion point when it is inserted into
// another drawing as a block or attached as an xref.
class BaseCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BASE"; }
    std::string_view description() const override {
        return "Set the insertion base point of this drawing";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        const Vec2 cur = xref::insertionBase(ctx.doc());
        auto p = co_await ctx.getPoint("Enter base point <" + xref::insertionBaseText(cur) + ">");
        if (p.status == cmd::Status::None) {
            ctx.message("Base point stays at " + xref::insertionBaseText(cur) + ".");
            co_return;
        }
        if (!p.ok())
            co_return;
        const std::string text = xref::insertionBaseText(p.point);
        (void)ctx.tx().modifyHeader([&](Header& h) {
            if (p.point == Vec2{})
                h.custom.erase("tbcad.insbase");
            else
                h.custom["tbcad.insbase"] = text;
        });
        ctx.setUndoLabel("BASE");
        ctx.message("Base point set to " + text +
                    ". It applies when this drawing is inserted or attached.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBaseCommand() {
    return std::make_unique<BaseCommand>();
}

std::unique_ptr<cmd::Command> makeAttredefCommand() {
    return std::make_unique<BlockCommand>(true);
}

std::unique_ptr<cmd::Command> makeBlockCommand() {
    return std::make_unique<BlockCommand>();
}

std::unique_ptr<cmd::Command> makeBlockLineCommand() {
    return std::make_unique<BlockCommand>(false, true);
}

}  // namespace tbcad::block_cmds
