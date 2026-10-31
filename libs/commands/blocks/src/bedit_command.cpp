#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "commands.h"
#include "tbcad/blocks/blocks.h"
#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/geom/geom.h"
#include "tbcad/xref/xref.h"

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
// Working copy -> the block entity it stands for ("copy:block,..."). Set by BEDIT for the copies
// it makes and by every save for the whole working set, so the parameters, actions and
// visibility states of a dynamic block (which name block entities) follow the edit.
constexpr const char* kPairsKey = "tmp.bedit.pairs";
constexpr const char* kHiddenKey = "tmp.bedit.hidden";  // copies hidden by the visibility state
constexpr const char* kTestKey = "tmp.bedit.test";      // BTESTBLOCK references
constexpr const char* kStateKey = "tmp.bedit.vstate";   // visibility state shown in the editor
constexpr const char* kSavedKey = "tmp.bedit.saved";    // the block was written during the edit
// The drawing's revision once the edit started or was last saved (the command's own commit
// included): BCLOSE with nothing changed since closes without asking Save or Discard.
constexpr const char* kRevKey = "tmp.bedit.rev";
constexpr const char* kPanelId = "block_authoring";

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

std::unordered_map<EntityId, EntityId> decodePairs(const std::string& s) {
    std::unordered_map<EntityId, EntityId> pairs;
    std::istringstream is(s);
    std::string tok;
    while (std::getline(is, tok, ',')) {
        const std::size_t colon = tok.find(':');
        if (colon == std::string::npos)
            continue;
        pairs[EntityId{std::strtoull(tok.c_str(), nullptr, 10)}] =
            EntityId{std::strtoull(tok.c_str() + colon + 1, nullptr, 10)};
    }
    return pairs;
}

std::string encodePairs(const std::vector<std::pair<EntityId, EntityId>>& pairs) {
    std::string s;
    for (const auto& [copy, block] : pairs)
        s +=
            (s.empty() ? "" : ",") + std::to_string(copy.value) + ":" + std::to_string(block.value);
    return s;
}

// Block entity ids named by a dynamic block or an assembly, renamed through `map`; ids that are
// not in it (objects deleted in the editor) are dropped.
void remapBlockIds(BlockDef& d, const std::unordered_map<EntityId, EntityId>& map) {
    auto remap = [&](std::vector<EntityId>& ids) {
        std::vector<EntityId> out;
        for (EntityId id : ids)
            if (auto it = map.find(id); it != map.end())
                out.push_back(it->second);
        ids = std::move(out);
    };
    if (d.dynamic) {
        for (DynParam& p : d.dynamic->params)
            for (auto& objs : p.stateObjects)
                remap(objs);
        for (DynAction& a : d.dynamic->actions)
            remap(a.objects);
    }
    if (d.assembly)
        for (AsmBinding& b : d.assembly->bindings)
            if (b.entity) {
                auto it = map.find(b.entity);
                b.entity = it == map.end() ? EntityId{} : it->second;
            }
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
    std::vector<EntityId> excl = decodeIds(host.setting(kExclKey).value_or(""));
    for (EntityId id : decodeIds(host.setting(kTestKey).value_or("")))
        excl.push_back(id);
    const std::unordered_set<EntityId> out(excl.begin(), excl.end());
    const std::vector<EntityId> drawn = doc.modelSpace();
    for (EntityId id : drawn)
        if (id.value > mark && !have.count(id) && !out.count(id))
            ids.push_back(id);
    // Back to front as they are drawn, so BSAVE keeps the draw order (DRAWORDER, TEXTTOFRONT,
    // HATCHTOBACK) in the block's own list.
    std::unordered_map<EntityId, std::size_t> rank;
    rank.reserve(drawn.size());
    for (std::size_t i = 0; i < drawn.size(); ++i)
        rank.emplace(drawn[i], i);
    std::stable_sort(ids.begin(), ids.end(), [&](EntityId a, EntityId b) {
        auto ia = rank.find(a), ib = rank.find(b);
        const std::size_t ra = ia == rank.end() ? drawn.size() : ia->second;
        const std::size_t rb = ib == rank.end() ? drawn.size() : ib->second;
        return ra < rb;
    });
    return ids;
}

void forget(cmd::IHost& host) {
    for (const char* k : {kBlockKey, kIdsKey, kXformKey, kMarkKey, kExclKey, kPairsKey, kHiddenKey,
                          kTestKey, kStateKey, kSavedKey, kRevKey})
        host.setSetting(k, "");
    host.closePanel(kPanelId);
}

// Erases the BTESTBLOCK references.
void eraseTests(cmd::CommandContext& ctx) {
    for (EntityId id : decodeIds(ctx.host().setting(kTestKey).value_or("")))
        if (ctx.doc().find(id))
            (void)ctx.tx().erase(id);
    ctx.host().setSetting(kTestKey, "");
}

// Writes the copies back into the definition. Returns false (with a message) when it cannot.
// `into` (another block, made by BSAVEAS with a copy of the definition) receives them instead.
bool saveBack(cmd::CommandContext& ctx, bool removeCopies, BlockId into = {}) {
    Document& doc = ctx.doc();
    const BlockDef* def = into ? doc.block(into) : doc.blockByName(editedBlock(ctx.host()));
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
    const std::vector<EntityId> hiddenList = decodeIds(ctx.host().setting(kHiddenKey).value_or(""));
    const std::unordered_set<EntityId> hidden(hiddenList.begin(), hiddenList.end());
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
        if (hidden.count(id))
            copy.common.visible = true;  // hidden only by the state shown in the editor
        content.push_back(std::move(copy));
    }
    for (const Entity& e : content)
        if (const auto* ins = entityAs<InsertData>(e))
            if (blocks::wouldCreateCycle(doc, block, ins->block)) {
                ctx.message("A block cannot contain itself, directly or through another block.");
                return false;
            }

    Transaction& tx = ctx.tx();
    if (!def->xrefPath.empty()) {
        // REFEDIT on an external reference: the edit goes into its file, then the reference is
        // reloaded from it.
        const auto hf = ctx.host().setting("document.file");
        const std::filesystem::path hostFile =
            hf ? std::filesystem::path(std::u8string(hf->begin(), hf->end()))
               : std::filesystem::path{};
        const std::size_t n = content.size();
        if (auto w = xref::writeBack(doc, block, content, hostFile); !w) {
            ctx.message(w.error().message);
            return false;
        }
        auto loaded = xref::loadStored(def->xrefPath, hostFile);
        if (!loaded) {
            ctx.message(loaded.error().message);
            return false;
        }
        const std::string name = def->name;
        // Copies go first, so that the reload can replace the nested blocks they insert.
        if (removeCopies)
            for (EntityId id : live)
                (void)tx.erase(id);
        if (auto r = xref::reload(tx, doc, block, loaded.value()); !r) {
            ctx.message(r.error().message);
            return false;
        }
        ctx.host().setSetting(kPairsKey, "");
        if (removeCopies) {
            forget(ctx.host());
        } else {
            ctx.host().setSetting(kSavedKey, "1");
        }
        ctx.message("Reference \"" + name + "\" saved to its file from " + std::to_string(n) +
                    " objects.");
        return true;
    }
    (void)tx.eraseAllIn(block);
    const auto pairs = decodePairs(ctx.host().setting(kPairsKey).value_or(""));
    std::unordered_map<EntityId, EntityId> renamed;  // old block entity -> new one
    std::vector<std::pair<EntityId, EntityId>> newPairs;
    for (std::size_t i = 0; i < content.size(); ++i) {
        auto r = tx.add(std::move(content[i]), block);
        if (!r) {
            ctx.message(r.error().message);
            return false;
        }
        if (auto it = pairs.find(live[i]); it != pairs.end())
            renamed[it->second] = r.value();
        newPairs.emplace_back(live[i], r.value());
    }
    if (def->dynamic || def->assembly)
        (void)tx.modifyBlock(block, [&](BlockDef& d) { remapBlockIds(d, renamed); });
    (void)blocks::syncInserts(tx, doc, block);
    const std::size_t n = content.size();
    if (removeCopies) {
        for (EntityId id : live)
            (void)tx.erase(id);
        eraseTests(ctx);
        forget(ctx.host());
    } else {
        ctx.host().setSetting(kPairsKey, encodePairs(newPairs));
        ctx.host().setSetting(kSavedKey, "1");
        ctx.host().setSetting(kRevKey, std::to_string(doc.revision() + 1));
    }
    ctx.message("Block \"" + doc.block(block)->name + "\" updated from " + std::to_string(n) +
                " objects.");
    return true;
}

}  // namespace

namespace detail {

bool startBlockEdit(cmd::CommandContext& ctx, const BlockDef& def, const Transform2& xf) {
    Document& doc = ctx.doc();
    if (!editedBlock(ctx.host()).empty()) {
        ctx.message("Block \"" + editedBlock(ctx.host()) +
                    "\" is already being edited. Use BSAVE or BCLOSE first.");
        return false;
    }
    if (!def.xrefPath.empty() && xref::isUnderlay(doc, def.id)) {
        ctx.message("A PDF or image underlay cannot be edited here.");
        return false;
    }
    ctx.commitOnCancel();
    std::uint64_t mark = 0;
    for (EntityId id : doc.modelSpace())
        mark = std::max(mark, id.value);
    ctx.host().setSetting(kMarkKey, std::to_string(mark));
    std::vector<EntityId> made;
    std::vector<std::pair<EntityId, EntityId>> pairs;
    const std::vector<EntityId> source = def.entities;
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
        if (added) {
            made.push_back(added.value());
            pairs.emplace_back(added.value(), id);
        }
    }
    ctx.host().setSetting(kPairsKey, encodePairs(pairs));
    ctx.host().setSetting(kBlockKey, def.name);
    ctx.host().setSetting(kIdsKey, encodeIds(made));
    ctx.host().setSetting(kXformKey, encodeXform(xf));
    ctx.host().setSetting(kRevKey, std::to_string(doc.revision() + 1));  // after this commit
    ctx.host().setSelection(made);
    ctx.setUndoLabel("Edit block " + def.name);
    ctx.message("Editing block \"" + def.name + "\" (" + std::to_string(made.size()) +
                " objects). Edit them, then BSAVE to update the block or BCLOSE to finish.");
    ctx.host().openPanel(kPanelId);
    return true;
}

std::optional<BlockEdit> blockEdit(const Document& doc, const cmd::IHost& host) {
    const BlockDef* def = doc.blockByName(editedBlock(host));
    if (!def)
        return std::nullopt;
    BlockEdit e;
    e.block = def->id;
    e.xf = decodeXform(host.setting(kXformKey).value_or(""));
    e.working = editedIds(doc, host);
    e.state = host.setting(kStateKey).value_or("");
    e.xref = !def->xrefPath.empty();
    return e;
}

bool saveBlockEdit(cmd::CommandContext& ctx, BlockId into) {
    if (!saveBack(ctx, false, into))
        return false;
    if (into)
        ctx.host().setSetting(kBlockKey, ctx.doc().block(into)->name);
    return true;
}

std::vector<EntityId> blockIdsOf(const cmd::IHost& host, const std::vector<EntityId>& copies) {
    const auto pairs = decodePairs(host.setting(kPairsKey).value_or(""));
    std::vector<EntityId> out;
    for (EntityId c : copies)
        if (auto it = pairs.find(c); it != pairs.end())
            out.push_back(it->second);
    return out;
}

std::vector<EntityId> copiesOf(const cmd::IHost& host, const std::vector<EntityId>& blockIds) {
    std::unordered_map<EntityId, EntityId> back;
    for (const auto& [copy, block] : decodePairs(host.setting(kPairsKey).value_or("")))
        back[block] = copy;
    std::vector<EntityId> out;
    for (EntityId b : blockIds)
        if (auto it = back.find(b); it != back.end())
            out.push_back(it->second);
    return out;
}

void showEditState(cmd::CommandContext& ctx, const std::string& state) {
    Document& doc = ctx.doc();
    const auto edit = blockEdit(doc, ctx.host());
    if (!edit)
        return;
    ctx.host().setSetting(kStateKey, state);
    ctx.host().setSetting(kRevKey, "");  // a visibility state was shown: BCLOSE asks again
    // What the state hides: objects named by some state but not by this one.
    std::unordered_set<EntityId> hideBlock;
    const BlockDef* def = doc.block(edit->block);
    if (def && def->dynamic)
        for (const DynParam& p : def->dynamic->params) {
            if (p.kind != DynParam::Kind::Visibility)
                continue;
            std::size_t at = p.states.size();
            for (std::size_t i = 0; i < p.states.size(); ++i)
                if (p.states[i] == state)
                    at = i;
            if (at == p.states.size())
                break;
            std::unordered_set<EntityId> shown(p.stateObjects[at].begin(),
                                               p.stateObjects[at].end());
            for (const auto& objs : p.stateObjects)
                for (EntityId id : objs)
                    if (!shown.count(id))
                        hideBlock.insert(id);
        }
    const auto pairs = decodePairs(ctx.host().setting(kPairsKey).value_or(""));
    std::vector<EntityId> hidden;
    for (EntityId copy : edit->working) {
        auto it = pairs.find(copy);
        const bool hide = it != pairs.end() && hideBlock.count(it->second);
        const Entity* e = doc.find(copy);
        if (!e)
            continue;
        if (e->common.visible == hide)
            (void)ctx.tx().modify(copy, [hide](Entity& en) { en.common.visible = !hide; });
        if (hide)
            hidden.push_back(copy);
    }
    ctx.host().setSetting(kHiddenKey, encodeIds(hidden));
}

void addTestReference(cmd::CommandContext& ctx, EntityId id) {
    std::vector<EntityId> ids = decodeIds(ctx.host().setting(kTestKey).value_or(""));
    ids.push_back(id);
    ctx.host().setSetting(kTestKey, encodeIds(ids));
}

std::vector<EntityId> testReferences(const cmd::IHost& host) {
    return decodeIds(host.setting(kTestKey).value_or(""));
}

void closeTestReferences(cmd::CommandContext& ctx) {
    eraseTests(ctx);
}

}  // namespace detail

namespace {

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
            // One block reference picked beforehand (a double click on it) is edited at once.
            eo.allowPreselection = true;
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
        (void)detail::startBlockEdit(ctx, *def, xf);
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
        const std::string rev = ctx.host().setting(kRevKey).value_or("");
        const bool untouched = !rev.empty() && rev == std::to_string(ctx.doc().revision());
        if (close_ && untouched) {
            // Nothing was changed since BEDIT or the last BSAVE: close as Save would (it writes
            // back what is already there) without asking.
            save = true;
        } else if (close_) {
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
        const bool savedBefore = ctx.host().setting(kSavedKey).value_or("") == "1";
        for (EntityId id : decodeIds(ctx.host().setting(kIdsKey).value_or("")))
            if (ctx.doc().find(id))
                (void)ctx.tx().erase(id);
        eraseTests(ctx);
        ctx.host().setSelection({});
        forget(ctx.host());
        ctx.setUndoLabel("Discard block edit");
        ctx.message(savedBefore ? "Block edit discarded. What was already saved into the block "
                                  "(BSAVE, parameters, visibility states) stays; UNDO takes it "
                                  "back."
                                : "Block edit discarded.");
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
        ctx.host().setSetting(kRevKey, "");  // the working set changed: BCLOSE asks again
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
