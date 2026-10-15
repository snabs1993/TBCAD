#include <cstdlib>
#include <sstream>

#include "commands.h"
#include "tbcad/blocks/blocks.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"

namespace tbcad::block_cmds {

namespace {

// In-place block editing. BEDIT copies the contents of a block into the drawing (at the position
// of a chosen insert, so it is edited in context); the user edits the copies with any command;
// BSAVE writes them back into the definition, which redraws every insert, and BCLOSE does the
// same and removes the copies (or discards them). What is being edited is remembered in
// in-memory settings.
constexpr const char* kBlockKey = "tmp.bedit.block";
constexpr const char* kIdsKey = "tmp.bedit.ids";
constexpr const char* kXformKey = "tmp.bedit.xform";

std::string encodeXform(const Transform2& t) {
    std::ostringstream os;
    os.precision(17);
    os << t.m00 << ' ' << t.m01 << ' ' << t.tx << ' ' << t.m10 << ' ' << t.m11 << ' ' << t.ty;
    return os.str();
}

Transform2 decodeXform(const std::string& s) {
    Transform2 t;
    std::istringstream is(s);
    is >> t.m00 >> t.m01 >> t.tx >> t.m10 >> t.m11 >> t.ty;
    return t;
}

std::string encodeIds(const std::vector<EntityId>& ids) {
    std::string s;
    for (EntityId id : ids)
        s += (s.empty() ? "" : ",") + std::to_string(id.value);
    return s;
}

std::vector<EntityId> decodeIds(const std::string& s) {
    std::vector<EntityId> ids;
    std::istringstream is(s);
    std::string tok;
    while (std::getline(is, tok, ','))
        if (!tok.empty())
            ids.push_back(EntityId{std::strtoull(tok.c_str(), nullptr, 10)});
    return ids;
}

std::string editedBlock(const cmd::IHost& host) {
    return host.setting(kBlockKey).value_or("");
}

void forget(cmd::IHost& host) {
    host.setSetting(kBlockKey, "");
    host.setSetting(kIdsKey, "");
    host.setSetting(kXformKey, "");
}

// Writes the copies back into the definition. Returns false (with a message) when it cannot.
bool saveBack(cmd::CommandContext& ctx, bool removeCopies) {
    Document& doc = ctx.doc();
    const BlockDef* def = doc.blockByName(editedBlock(ctx.host()));
    if (!def) {
        ctx.message("The block being edited no longer exists.");
        forget(ctx.host());
        return false;
    }
    const BlockId block = def->id;
    auto inv = decodeXform(ctx.host().setting(kXformKey).value_or("")).inverse();
    if (!inv) {
        ctx.message("Cannot write the block back: its reference cannot be undone (zero scale).");
        return false;
    }
    std::vector<Entity> content;
    std::vector<EntityId> live;
    for (EntityId id : decodeIds(ctx.host().setting(kIdsKey).value_or(""))) {
        const Entity* e = doc.find(id);
        if (!e)
            continue;
        live.push_back(id);
        auto back = geom::transformed(*e, inv.value());
        if (!back) {
            ctx.message(back.error().message);
            return false;
        }
        Entity copy = std::move(back.value());
        copy.common.id = {};
        content.push_back(std::move(copy));
    }
    for (const Entity& e : content)
        if (const auto* ins = entityAs<InsertData>(e))
            if (blocks::wouldCreateCycle(doc, block, ins->block)) {
                ctx.message("A block cannot contain itself, directly or through another block.");
                return false;
            }

    Transaction& tx = ctx.tx();
    const std::vector<EntityId> old = doc.block(block)->entities;
    for (EntityId id : old)
        (void)tx.erase(id);
    for (Entity& e : content) {
        auto r = tx.add(std::move(e), block);
        if (!r) {
            ctx.message(r.error().message);
            return false;
        }
    }
    (void)blocks::syncInserts(tx, doc, block);
    if (removeCopies) {
        for (EntityId id : live)
            (void)tx.erase(id);
        forget(ctx.host());
    }
    ctx.message("Block \"" + doc.block(block)->name + "\" updated from " +
                std::to_string(content.size()) + " objects.");
    return true;
}

class BEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"BE"}; }
    std::string_view description() const override {
        return "Edit a block in place; finish with BSAVE or BCLOSE";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        if (!editedBlock(ctx.host()).empty()) {
            ctx.message("Block \"" + editedBlock(ctx.host()) +
                        "\" is already being edited. Use BSAVE or BCLOSE first.");
            co_return;
        }
        if (doc.blocks().empty()) {
            ctx.message("There are no blocks in this drawing.");
            co_return;
        }
        const BlockDef* def = nullptr;
        Transform2 xf = Transform2::identity();
        for (;;) {
            cmd::EntityOptions eo;
            eo.keywords = {"Name"};
            eo.filter = [](const Entity& e) { return kind(e) == EntityKind::Insert; };
            auto pick = co_await ctx.getEntity("Select a block reference or [Name]", eo);
            if (pick.status == cmd::Status::Keyword) {
                auto n = co_await ctx.getString("Enter block name");
                if (!n.ok())
                    co_return;
                def = doc.blockByName(cmd::trim(n.text));
                if (!def) {
                    ctx.message("No block named \"" + cmd::trim(n.text) + "\".");
                    continue;
                }
                xf = Transform2::translate(def->base * -1.0);
                break;
            }
            if (!pick.ok())
                co_return;
            const Entity* e = doc.find(pick.id);
            const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
            def = ins ? doc.block(ins->block) : nullptr;
            if (!def)
                continue;
            xf = blocks::instanceTransform(doc, *ins);
            break;
        }
        if (!def->xrefPath.empty()) {
            ctx.message("An external reference cannot be edited here.");
            co_return;
        }
        ctx.commitOnCancel();
        std::vector<EntityId> made;
        const std::vector<EntityId> source = def->entities;
        for (EntityId id : source) {
            const Entity* e = doc.find(id);
            if (!e)
                continue;
            auto moved = geom::transformed(*e, xf);
            if (!moved) {
                ctx.message(moved.error().message);
                continue;
            }
            Entity copy = std::move(moved.value());
            copy.common.id = {};
            auto added = ctx.tx().add(std::move(copy));
            if (added)
                made.push_back(added.value());
        }
        ctx.host().setSetting(kBlockKey, def->name);
        ctx.host().setSetting(kIdsKey, encodeIds(made));
        ctx.host().setSetting(kXformKey, encodeXform(xf));
        ctx.host().setSelection(made);
        ctx.setUndoLabel("Edit block " + def->name);
        ctx.message("Editing block \"" + def->name + "\" (" + std::to_string(made.size()) +
                    " objects). Edit them, then BSAVE to update the block or BCLOSE to finish.");
    }
};

class BSaveCommand final : public cmd::Command {
public:
    explicit BSaveCommand(bool close) : close_(close) {}
    std::string_view name() const override { return close_ ? "BCLOSE" : "BSAVE"; }
    std::vector<std::string> defaultAliases() const override {
        return close_ ? std::vector<std::string>{"BC"} : std::vector<std::string>{};
    }
    std::string_view description() const override {
        return close_ ? "Finish editing a block: save the changes or discard them"
                      : "Write the edited objects back into the block";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        if (editedBlock(ctx.host()).empty()) {
            ctx.message("No block is being edited. Use BEDIT first.");
            co_return;
        }
        bool save = true;
        if (close_) {
            auto k = co_await ctx.getKeyword(
                "Save the changes to the block [Save/Discard] <Save>",
                cmd::KeywordOptions{.keywords = {"Save", "Discard"}, .defaultKeyword = "Save"});
            if (!k.ok())
                co_return;
            save = k.keyword == "Save";
        }
        if (save) {
            ctx.setUndoLabel(close_ ? "Close block edit" : "Save block");
            (void)saveBack(ctx, close_);
            co_return;
        }
        for (EntityId id : decodeIds(ctx.host().setting(kIdsKey).value_or("")))
            if (ctx.doc().find(id))
                (void)ctx.tx().erase(id);
        ctx.host().setSelection({});
        forget(ctx.host());
        ctx.setUndoLabel("Discard block edit");
        ctx.message("Block edit discarded.");
    }

private:
    bool close_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeBEditCommand() {
    return std::make_unique<BEditCommand>();
}
std::unique_ptr<cmd::Command> makeBSaveCommand(bool close) {
    return std::make_unique<BSaveCommand>(close);
}

}  // namespace tbcad::block_cmds
