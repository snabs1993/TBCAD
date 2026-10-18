#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

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
constexpr const char* kMarkKey = "tmp.bedit.mark";  // highest object id before BEDIT
constexpr const char* kExclKey = "tmp.bedit.excl";  // drawn since BEDIT but taken out by REFSET

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

// The copies made by BEDIT plus every object drawn since (their ids are above the mark).
std::vector<EntityId> editedIds(const Document& doc, const cmd::IHost& host) {
    std::vector<EntityId> ids = decodeIds(host.setting(kIdsKey).value_or(""));
    const std::uint64_t mark =
        std::strtoull(host.setting(kMarkKey).value_or("0").c_str(), nullptr, 10);
    std::unordered_set<EntityId> have(ids.begin(), ids.end());
    const std::vector<EntityId> excl = decodeIds(host.setting(kExclKey).value_or(""));
    const std::unordered_set<EntityId> out(excl.begin(), excl.end());
    for (EntityId id : doc.modelSpace())
        if (id.value > mark && !have.count(id) && !out.count(id))
            ids.push_back(id);
    return ids;
}

void forget(cmd::IHost& host) {
    host.setSetting(kBlockKey, "");
    host.setSetting(kIdsKey, "");
    host.setSetting(kXformKey, "");
    host.setSetting(kMarkKey, "");
    host.setSetting(kExclKey, "");
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
    for (EntityId id : editedIds(doc, ctx.host())) {
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
    std::vector<std::string> defaultAliases() const override { return {"BE", "REFEDIT"}; }
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
            // A block name typed at the prompt works as well as the Name option.
            for (const auto& [id, b] : doc.blocks())
                if (!b.name.empty() && b.name.front() != '*' && eo.keywords.size() < 400)
                    eo.keywords.push_back(b.name);
            eo.filter = [](const Entity& e) { return kind(e) == EntityKind::Insert; };
            auto pick = co_await ctx.getEntity("Select a block reference or [Name]", eo);
            if (pick.status == cmd::Status::Keyword) {
                std::string wanted = pick.keyword;
                if (wanted == "Name") {
                    auto n = co_await ctx.getString("Enter block name");
                    if (!n.ok())
                        co_return;
                    wanted = cmd::trim(n.text);
                }
                def = doc.blockByName(wanted);
                if (!def) {
                    ctx.message("No block named \"" + wanted + "\".");
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
        std::uint64_t mark = 0;
        for (EntityId id : doc.modelSpace())
            mark = std::max(mark, id.value);
        ctx.host().setSetting(kMarkKey, std::to_string(mark));
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
        return close_ ? std::vector<std::string>{"BC", "REFCLOSE"} : std::vector<std::string>{};
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
                "Closing block \"" + editedBlock(ctx.host()) +
                    "\": save the changes to the block or throw them away [Save/Discard] <Save>",
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

// REFSET: changes the working set of the block being edited. Add pulls drawing objects into the
// set (BSAVE moves them into the block), Remove leaves them in the drawing.
class RefSetCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "REFSET"; }
    std::string_view description() const override {
        return "Add objects to or remove them from the block being edited";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        if (editedBlock(ctx.host()).empty()) {
            ctx.message("No block is being edited. Use BEDIT first.");
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"Add", "Remove"};
        ko.defaultKeyword = "Add";
        auto k = co_await ctx.getKeyword("Enter an option [Add/Remove] <Add>", std::move(ko));
        if (!k.ok())
            co_return;
        const bool add = k.keyword != "Remove";
        auto sel = co_await ctx.getSelection(add ? "Select objects to add to the working set"
                                                 : "Select objects to remove from the working set");
        if (!sel.ok())
            co_return;
        std::vector<EntityId> ids = decodeIds(ctx.host().setting(kIdsKey).value_or(""));
        std::vector<EntityId> excl = decodeIds(ctx.host().setting(kExclKey).value_or(""));
        const std::vector<EntityId> current = editedIds(doc, ctx.host());
        const std::unordered_set<EntityId> inSet(current.begin(), current.end());
        auto drop = [](std::vector<EntityId>& v, EntityId id) {
            v.erase(std::remove(v.begin(), v.end(), id), v.end());
        };
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            if (!doc.find(id))
                continue;
            if (add && !inSet.count(id)) {
                ids.push_back(id);
                drop(excl, id);
                ++n;
            } else if (!add && inSet.count(id)) {
                drop(ids, id);
                excl.push_back(id);
                ++n;
            }
        }
        ctx.host().setSetting(kIdsKey, encodeIds(ids));
        ctx.host().setSetting(kExclKey, encodeIds(excl));
        ctx.message(std::to_string(n) + (add ? " objects added to" : " objects removed from") +
                    " the working set.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeRefSetCommand() {
    return std::make_unique<RefSetCommand>();
}

std::unique_ptr<cmd::Command> makeBEditCommand() {
    return std::make_unique<BEditCommand>();
}
std::unique_ptr<cmd::Command> makeBSaveCommand(bool close) {
    return std::make_unique<BSaveCommand>(close);
}

}  // namespace tbcad::block_cmds
