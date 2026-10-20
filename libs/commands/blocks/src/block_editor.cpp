// The block editor's commands besides BEDIT / BSAVE / BCLOSE / REFSET (bedit_command.cpp):
// BSAVEAS, visibility states (BVSTATE, BVHIDE, BVSHOW), BLOOKUPTABLE, BTESTBLOCK, BATTORDER, and
// the Block Authoring panel that BEDIT opens.
#include <algorithm>
#include <sstream>
#include <unordered_set>

#include "commands.h"
#include "tbcad/blocks/tbcad_blocks.h"

#ifdef TBCAD_BLOCK_PANEL_IMGUI
#include <imgui.h>
#endif

namespace tbcad::block_cmds {

namespace {

// The block editor's own commands need a block being edited, and not an external reference.
bool needEdit(cmd::CommandContext& ctx) {
    const auto edit = detail::blockEdit(ctx.doc(), ctx.host());
    if (!edit) {
        ctx.message("No block is being edited. Use BEDIT first.");
        return false;
    }
    if (edit->xref) {
        ctx.message("An external reference is being edited: change its objects, then REFCLOSE.");
        return false;
    }
    return true;
}

// Index of the block's visibility parameter, or -1.
int visibilityParam(const BlockDef& def) {
    if (!def.dynamic)
        return -1;
    for (std::size_t i = 0; i < def.dynamic->params.size(); ++i)
        if (def.dynamic->params[i].kind == DynParam::Kind::Visibility)
            return static_cast<int>(i);
    return -1;
}

int stateIndex(const DynParam& p, const std::string& name) {
    for (std::size_t i = 0; i < p.states.size(); ++i)
        if (p.states[i] == name)
            return static_cast<int>(i);
    return -1;
}

std::string joined(const std::vector<std::string>& v, const char* sep = "/") {
    std::string s;
    for (const std::string& x : v)
        s += (s.empty() ? "" : sep) + x;
    return s;
}

// The state shown in the editor, or the first one.
std::string currentState(const detail::BlockEdit& edit, const DynParam& p) {
    if (stateIndex(p, edit.state) >= 0)
        return edit.state;
    return p.states.empty() ? std::string() : p.states.front();
}

// Every object of the block in at least one state, so that a state can hide any of them
// (objects named by no state are shown in all states).
void nameEverywhere(DynParam& p, const std::vector<EntityId>& all) {
    std::unordered_set<EntityId> named;
    for (const auto& objs : p.stateObjects)
        named.insert(objs.begin(), objs.end());
    for (EntityId id : all)
        if (!named.count(id))
            for (auto& objs : p.stateObjects)
                objs.push_back(id);
}

// ---- BSAVEAS ---------------------------------------------------------------------------------

class BSaveAsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BSAVEAS"; }
    std::string_view description() const override {
        return "Save the block being edited as a new block and go on editing that one";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        if (!needEdit(ctx))
            co_return;
        auto n = co_await ctx.getString("Enter the name of the new block");
        if (!n.ok())
            co_return;
        const std::string name = cmd::trim(n.text);
        if (name.empty() || name.front() == '*') {
            ctx.message("Enter a block name.");
            co_return;
        }
        if (ctx.doc().blockByName(name)) {
            ctx.message("A block called \"" + name + "\" already exists.");
            co_return;
        }
        const BlockDef* from = ctx.doc().block(detail::blockEdit(ctx.doc(), ctx.host())->block);
        BlockDef row;
        row.name = name;
        row.base = from->base;
        row.dynamic = from->dynamic;  // names the old block's objects: the save renames them
        row.assembly = from->assembly;
        auto id = ctx.tx().addBlock(std::move(row));
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        if (!detail::saveBlockEdit(ctx, id.value()))
            co_return;
        ctx.setUndoLabel("Save block as " + name);
        ctx.message("Now editing block \"" + name + "\".");
    }
};

// ---- BVSTATE ---------------------------------------------------------------------------------

class BvStateCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BVSTATE"; }
    std::string_view description() const override {
        return "Visibility states of the block being edited: new, set current, rename, delete";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        if (!needEdit(ctx))
            co_return;
        Document& doc = ctx.doc();
        const auto edit = detail::blockEdit(doc, ctx.host());
        const BlockDef* def = doc.block(edit->block);
        const int vi = visibilityParam(*def);
        std::vector<std::string> states;
        if (vi >= 0)
            states = def->dynamic->params[static_cast<std::size_t>(vi)].states;
        cmd::KeywordOptions ko;
        ko.keywords = vi >= 0 ? std::vector<std::string>{"New", "Set", "Rename", "Delete"}
                              : std::vector<std::string>{"New"};
        ko.defaultKeyword = vi >= 0 ? "Set" : "New";
        auto k = co_await ctx.getKeyword(
            vi >= 0 ? "Visibility states: " + joined(states, ", ") +
                          ". Enter an option [New/Set/Rename/Delete] <Set>"
                    : "The block has no visibility states. Enter an option [New] <New>",
            std::move(ko));
        if (!k.ok())
            co_return;

        if (k.keyword == "New") {
            auto n = co_await ctx.getString("Enter the name of the new visibility state");
            if (!n.ok())
                co_return;
            const std::string stateName = cmd::trim(n.text);
            if (stateName.empty())
                co_return;
            if (std::find(states.begin(), states.end(), stateName) != states.end()) {
                ctx.message("There is already a state called \"" + stateName + "\".");
                co_return;
            }
            auto mode = co_await ctx.getKeyword(
                "Objects in the new state [Hide all/Show all/Current] <Current>",
                cmd::KeywordOptions{.keywords = {"Hide", "Show", "Current"},
                                    .defaultKeyword = "Current"});
            if (!mode.ok())
                co_return;
            if (!detail::saveBlockEdit(ctx))
                co_return;
            const BlockDef* saved = doc.block(edit->block);
            const std::vector<EntityId> all = saved->entities;
            const std::string shown =
                vi >= 0 ? currentState(*edit, saved->dynamic->params[vi]) : std::string();
            (void)ctx.tx().modifyBlock(edit->block, [&](BlockDef& d) {
                if (!d.dynamic)
                    d.dynamic = DynamicDef{};
                int at = visibilityParam(d);
                if (at < 0) {
                    // The first state shows everything there is now.
                    DynParam p;
                    p.kind = DynParam::Kind::Visibility;
                    p.name = "Visibility";
                    p.states = {"VisibilityState0"};
                    p.stateObjects = {all};
                    d.dynamic->params.push_back(std::move(p));
                    at = static_cast<int>(d.dynamic->params.size()) - 1;
                }
                DynParam& p = d.dynamic->params[static_cast<std::size_t>(at)];
                nameEverywhere(p, all);
                std::vector<EntityId> objs;
                if (mode.keyword == "Show") {
                    objs = all;
                } else if (mode.keyword == "Current") {
                    const int cur = stateIndex(p, shown.empty() ? p.states.front() : shown);
                    objs = cur >= 0 ? p.stateObjects[static_cast<std::size_t>(cur)] : all;
                }
                p.states.push_back(stateName);
                p.stateObjects.push_back(std::move(objs));
            });
            detail::showEditState(ctx, stateName);
            ctx.setUndoLabel("New visibility state");
            ctx.message("Visibility state \"" + stateName + "\" added and shown.");
            co_return;
        }

        auto pickState = [&](const std::string& prompt) {
            cmd::KeywordOptions so;
            so.keywords = states;
            so.defaultKeyword = currentState(*edit, def->dynamic->params[vi]);
            return ctx.getKeyword(prompt + " [" + joined(states) + "] <" + *so.defaultKeyword + ">",
                                  std::move(so));
        };
        if (k.keyword == "Set") {
            auto st = co_await pickState("Visibility state to show");
            if (!st.ok())
                co_return;
            detail::showEditState(ctx, st.keyword);
            ctx.setUndoLabel("Show visibility state");
            co_return;
        }
        if (k.keyword == "Rename") {
            auto st = co_await pickState("Visibility state to rename");
            if (!st.ok())
                co_return;
            auto n = co_await ctx.getString("Enter the new name");
            if (!n.ok())
                co_return;
            const std::string to = cmd::trim(n.text);
            if (to.empty() || std::find(states.begin(), states.end(), to) != states.end()) {
                ctx.message("That name is empty or already used.");
                co_return;
            }
            (void)ctx.tx().modifyBlock(edit->block, [&](BlockDef& d) {
                DynParam& p = d.dynamic->params[static_cast<std::size_t>(vi)];
                p.states[static_cast<std::size_t>(stateIndex(p, st.keyword))] = to;
            });
            if (edit->state == st.keyword)
                detail::showEditState(ctx, to);
            ctx.setUndoLabel("Rename visibility state");
            co_return;
        }
        // Delete.
        if (states.size() < 2) {
            ctx.message("The last visibility state cannot be deleted.");
            co_return;
        }
        auto st = co_await pickState("Visibility state to delete");
        if (!st.ok())
            co_return;
        const std::string gone = st.keyword;
        const std::string shownBefore = currentState(*edit, def->dynamic->params[vi]);
        (void)ctx.tx().modifyBlock(edit->block, [&](BlockDef& d) {
            DynParam& p = d.dynamic->params[static_cast<std::size_t>(vi)];
            const auto at = static_cast<std::size_t>(stateIndex(p, gone));
            p.states.erase(p.states.begin() + static_cast<std::ptrdiff_t>(at));
            p.stateObjects.erase(p.stateObjects.begin() + static_cast<std::ptrdiff_t>(at));
        });
        detail::showEditState(
            ctx, shownBefore != gone ? shownBefore
                                     : doc.block(edit->block)->dynamic->params[vi].states.front());
        ctx.setUndoLabel("Delete visibility state");
        ctx.message("Visibility state \"" + gone + "\" deleted.");
    }
};

// ---- BVHIDE / BVSHOW -------------------------------------------------------------------------

class BvHideShowCommand final : public cmd::Command {
public:
    explicit BvHideShowCommand(bool show) : show_(show) {}
    std::string_view name() const override { return show_ ? "BVSHOW" : "BVHIDE"; }
    std::string_view description() const override {
        return show_ ? "Show objects in the current visibility state of the block being edited"
                     : "Hide objects in the current visibility state of the block being edited";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        if (!needEdit(ctx))
            co_return;
        Document& doc = ctx.doc();
        const auto edit = detail::blockEdit(doc, ctx.host());
        const BlockDef* def = doc.block(edit->block);
        const int vi = visibilityParam(*def);
        if (vi < 0) {
            ctx.message("The block has no visibility states. Use BVSTATE New first.");
            co_return;
        }
        const std::string state = currentState(*edit, def->dynamic->params[vi]);
        const std::unordered_set<EntityId> working(edit->working.begin(), edit->working.end());
        // BVSHOW: the hidden objects are shown while they are picked.
        if (show_)
            for (EntityId id : edit->working)
                if (const Entity* e = doc.find(id); e && !e->common.visible)
                    (void)ctx.tx().modify(id, [](Entity& en) { en.common.visible = true; });
        cmd::SelectionOptions so;
        so.filter = [&working](const Entity& e) { return working.count(e.common.id) > 0; };
        so.allowPreselection = !show_;
        if (show_)
            so.keywords = {"All"};
        auto sel = co_await ctx.getSelection(
            std::string(show_ ? "Select objects to show" : "Select objects to hide") +
                " in state \"" + state + "\"" + (show_ ? " or [All]" : ""),
            so);
        const bool all = sel.status == cmd::Status::Keyword && sel.keyword == "All";
        if (!all && (!sel.ok() || sel.ids.empty())) {
            detail::showEditState(ctx, state);
            co_return;
        }
        if (!detail::saveBlockEdit(ctx))
            co_return;
        const std::vector<EntityId> picked =
            all ? doc.block(edit->block)->entities : detail::blockIdsOf(ctx.host(), sel.ids);
        const std::vector<EntityId> everything = doc.block(edit->block)->entities;
        (void)ctx.tx().modifyBlock(edit->block, [&](BlockDef& d) {
            DynParam& p = d.dynamic->params[static_cast<std::size_t>(vi)];
            nameEverywhere(p, everything);
            auto& objs = p.stateObjects[static_cast<std::size_t>(stateIndex(p, state))];
            for (EntityId id : picked) {
                const bool in = std::find(objs.begin(), objs.end(), id) != objs.end();
                if (show_ && !in)
                    objs.push_back(id);
                else if (!show_ && in)
                    objs.erase(std::remove(objs.begin(), objs.end(), id), objs.end());
            }
        });
        detail::showEditState(ctx, state);
        ctx.setUndoLabel(show_ ? "Show in visibility state" : "Hide in visibility state");
        ctx.message(detail::objects(picked.size()) + (show_ ? " shown" : " hidden") +
                    " in state \"" + state + "\".");
    }

private:
    bool show_;
};

// ---- BLOOKUPTABLE ----------------------------------------------------------------------------

std::string number(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

class BLookupTableCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BLOOKUPTABLE"; }
    std::string_view description() const override {
        return "Edit the rows of a lookup table of a dynamic block";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        BlockId block{};
        if (auto edit = detail::blockEdit(doc, ctx.host())) {
            block = edit->block;
        } else {
            auto n = co_await ctx.getString("Enter block name");
            if (!n.ok())
                co_return;
            const BlockDef* def = doc.blockByName(cmd::trim(n.text));
            if (!def) {
                ctx.message("There is no block called \"" + cmd::trim(n.text) + "\"");
                co_return;
            }
            block = def->id;
        }
        std::vector<std::string> tables;
        if (const BlockDef* def = doc.block(block); def->dynamic)
            for (const DynParam& p : def->dynamic->params)
                if (p.kind == DynParam::Kind::Lookup)
                    tables.push_back(p.name);
        if (tables.empty()) {
            ctx.message("The block has no lookup table. Add one with BPARAM Table.");
            co_return;
        }
        std::string table = tables.front();
        if (tables.size() > 1) {
            auto t = co_await ctx.getKeyword(
                "Lookup table [" + joined(tables) + "]",
                cmd::KeywordOptions{.keywords = tables, .defaultKeyword = tables.front()});
            if (!t.ok())
                co_return;
            table = t.keyword;
        }
        bool changed = false;
        for (;;) {
            const BlockDef* def = doc.block(block);
            const int pi = blocks::dynFind(*def->dynamic, table);
            const DynParam& p = def->dynamic->params[static_cast<std::size_t>(pi)];
            auto k = co_await ctx.getKeyword(
                "Lookup table \"" + table + "\": " + std::to_string(p.states.size()) +
                    " rows. Enter an option [List/Add/Delete/Value/eXit] <eXit>",
                cmd::KeywordOptions{.keywords = {"List", "Add", "Delete", "Value", "eXit"},
                                    .defaultKeyword = "eXit"});
            if (!k.ok() || k.keyword == "eXit")
                break;
            if (k.keyword == "List") {
                std::string head = "Row";
                for (int at : p.lookupParams)
                    head += " | " + def->dynamic->params[static_cast<std::size_t>(at)].name;
                ctx.message(head);
                for (std::size_t r = 0; r < p.states.size(); ++r) {
                    std::string line = p.states[r];
                    for (double v : p.lookupValues[r])
                        line += " | " + number(v);
                    ctx.message(line);
                }
                continue;
            }
            const std::vector<int> drives = p.lookupParams;
            std::vector<std::string> driveNames;
            for (int at : drives)
                driveNames.push_back(def->dynamic->params[static_cast<std::size_t>(at)].name);
            const std::vector<std::string> rows = p.states;
            if (k.keyword == "Add") {
                auto n = co_await ctx.getString("Enter the name of the new row");
                if (!n.ok())
                    continue;
                const std::string row = cmd::trim(n.text);
                if (row.empty() || std::find(rows.begin(), rows.end(), row) != rows.end()) {
                    ctx.message("That name is empty or already used.");
                    continue;
                }
                std::vector<double> vals;
                bool ok = true;
                for (const std::string& dn : driveNames) {
                    auto v = co_await ctx.getReal("Value of " + dn + " in \"" + row + "\"");
                    if (!v.ok()) {
                        ok = false;
                        break;
                    }
                    vals.push_back(v.value);
                }
                if (!ok)
                    continue;
                (void)ctx.tx().modifyBlock(block, [&](BlockDef& d) {
                    DynParam& q = d.dynamic->params[static_cast<std::size_t>(pi)];
                    q.states.push_back(row);
                    q.stateObjects.emplace_back();
                    q.lookupValues.push_back(vals);
                });
                changed = true;
                continue;
            }
            auto r = co_await ctx.getKeyword(
                "Row [" + joined(rows) + "]",
                cmd::KeywordOptions{.keywords = rows, .defaultKeyword = rows.front()});
            if (!r.ok())
                continue;
            const auto row = static_cast<std::size_t>(
                std::find(rows.begin(), rows.end(), r.keyword) - rows.begin());
            if (k.keyword == "Delete") {
                if (rows.size() <= 2) {
                    ctx.message("A lookup table needs at least two rows.");
                    continue;
                }
                (void)ctx.tx().modifyBlock(block, [&](BlockDef& d) {
                    DynParam& q = d.dynamic->params[static_cast<std::size_t>(pi)];
                    const auto at = static_cast<std::ptrdiff_t>(row);
                    q.states.erase(q.states.begin() + at);
                    q.stateObjects.erase(q.stateObjects.begin() + at);
                    q.lookupValues.erase(q.lookupValues.begin() + at);
                });
                changed = true;
                continue;
            }
            // Value.
            std::size_t col = 0;
            if (driveNames.size() > 1) {
                auto c = co_await ctx.getKeyword(
                    "Parameter [" + joined(driveNames) + "]",
                    cmd::KeywordOptions{.keywords = driveNames,
                                        .defaultKeyword = driveNames.front()});
                if (!c.ok())
                    continue;
                col = static_cast<std::size_t>(
                    std::find(driveNames.begin(), driveNames.end(), c.keyword) -
                    driveNames.begin());
            }
            const double cur = p.lookupValues[row][col];
            auto v = co_await ctx.getReal(
                "Value of " + driveNames[col] + " in \"" + rows[row] + "\" <" + number(cur) + ">",
                cmd::RealOptions{.defaultValue = cur});
            if (!v.ok())
                continue;
            (void)ctx.tx().modifyBlock(block, [&](BlockDef& d) {
                d.dynamic->params[static_cast<std::size_t>(pi)].lookupValues[row][col] = v.value;
            });
            changed = true;
        }
        if (changed) {
            (void)blocks::syncInserts(ctx.tx(), doc, block);
            ctx.setUndoLabel("Edit lookup table");
        }
    }
};

// ---- BTESTBLOCK ------------------------------------------------------------------------------

class BTestBlockCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BTESTBLOCK"; }
    std::string_view description() const override {
        return "Try the block being edited: save it and place a test reference";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        if (!needEdit(ctx))
            co_return;
        if (!detail::testReferences(ctx.host()).empty()) {
            auto k = co_await ctx.getKeyword(
                "Test references are in the drawing. [Insert another/Close test] <Close test>",
                cmd::KeywordOptions{.keywords = {"Insert", "Close"}, .defaultKeyword = "Close"});
            if (!k.ok())
                co_return;
            if (k.keyword == "Close") {
                detail::closeTestReferences(ctx);
                ctx.setUndoLabel("Close block test");
                ctx.message("Test references removed.");
                co_return;
            }
        }
        if (!detail::saveBlockEdit(ctx))
            co_return;
        const auto edit = detail::blockEdit(ctx.doc(), ctx.host());
        auto at = co_await ctx.getPoint("Specify the insertion point of the test reference");
        if (!at.ok())
            co_return;
        InsertData ins;
        ins.block = edit->block;
        ins.position = at.point;
        auto id = ctx.tx().add(Entity{{}, std::move(ins)});
        if (!id) {
            ctx.message(id.error().message);
            co_return;
        }
        detail::addTestReference(ctx, id.value());
        ctx.host().setSelection({id.value()});
        ctx.setUndoLabel("Test block");
        ctx.message(
            "Try the test reference with its grips or DYNSET. BTESTBLOCK Close or BCLOSE "
            "removes it.");
    }
};

// ---- BATTORDER -------------------------------------------------------------------------------

const AttDefInfo* attdefOf(const Entity& e) {
    const auto* t = entityAs<TextData>(e);
    return t && t->attdef ? &*t->attdef : nullptr;
}

class BAttOrderCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BATTORDER"; }
    std::string_view description() const override {
        return "Set the order in which a block's attributes are asked for and listed";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        Document& doc = ctx.doc();
        const auto edit = detail::blockEdit(doc, ctx.host());
        BlockId block{};
        std::vector<EntityId> pool;  // where the attribute definitions are, back to front
        if (edit) {
            block = edit->block;
            pool = edit->working;
        } else {
            auto n = co_await ctx.getString("Enter block name");
            if (!n.ok())
                co_return;
            const BlockDef* def = doc.blockByName(cmd::trim(n.text));
            if (!def) {
                ctx.message("There is no block called \"" + cmd::trim(n.text) + "\"");
                co_return;
            }
            block = def->id;
            pool = def->entities;
        }
        std::vector<EntityId> atts;
        std::vector<std::string> tags;
        for (EntityId id : pool)
            if (const Entity* e = doc.find(id))
                if (const AttDefInfo* a = attdefOf(*e)) {
                    atts.push_back(id);
                    tags.push_back(a->tag);
                }
        if (atts.size() < 2) {
            ctx.message("The block has fewer than two attributes.");
            co_return;
        }
        const std::string now = joined(tags, ",");
        auto order = co_await ctx.getString(
            "Attributes in order: " + now + ". Enter the tags in the new order <" + now + ">",
            cmd::StringOptions{.defaultValue = now});
        if (!order.ok())
            co_return;
        std::vector<EntityId> sorted;
        std::vector<bool> used(atts.size(), false);
        std::istringstream in(order.text);
        for (std::string tok; std::getline(in, tok, ',');) {
            std::string tag = cmd::trim(tok);
            std::transform(tag.begin(), tag.end(), tag.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (tag.empty())
                continue;
            std::size_t i = 0;
            while (i < tags.size() && (used[i] || tags[i] != tag))
                ++i;
            if (i == tags.size()) {
                ctx.message("There is no attribute tagged \"" + tag +
                            "\" (or it is listed twice).");
                co_return;
            }
            used[i] = true;
            sorted.push_back(atts[i]);
        }
        for (std::size_t i = 0; i < atts.size(); ++i)  // the ones not named keep their order
            if (!used[i])
                sorted.push_back(atts[i]);
        if (sorted == atts) {
            ctx.message("The order is unchanged.");
            co_return;
        }
        if (edit) {
            // The working set is saved back to front: the attributes go on top in the new order.
            for (EntityId id : sorted)
                (void)ctx.tx().bringToFront(id);
        } else {
            // The attributes take the places they had in the block, in the new order.
            std::vector<EntityId> order2 = doc.block(block)->entities;
            std::size_t k = 0;
            for (EntityId& id : order2)
                if (std::find(atts.begin(), atts.end(), id) != atts.end())
                    id = sorted[k++];
            auto r = ctx.tx().setDrawOrder(order2, block);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
        }
        ctx.setUndoLabel("Attribute order");
        ctx.message(edit ? "Attribute order changed; BSAVE writes it into the block."
                         : "Attribute order changed.");
    }
};

#ifdef TBCAD_BLOCK_PANEL_IMGUI

const char* kindName(DynParam::Kind k) {
    switch (k) {
        case DynParam::Kind::Linear:
            return "Linear";
        case DynParam::Kind::Rotation:
            return "Rotation";
        case DynParam::Kind::Flip:
            return "Flip";
        case DynParam::Kind::Visibility:
            return "Visibility";
        case DynParam::Kind::Lookup:
            return "Lookup";
    }
    return "";
}

// The Block Authoring panel: what BEDIT is editing, its parameters, actions and visibility
// states, with buttons for the block editor's commands.
void drawAuthoring(ui::PanelContext& pc) {
    if (!pc.doc || !pc.host)
        return;
    const auto edit = detail::blockEdit(*pc.doc, *pc.host);
    if (!edit) {
        ImGui::TextWrapped("No block is being edited. BEDIT opens a block here.");
        if (ImGui::Button("Edit a block...") && pc.runCommand)
            pc.runCommand("BEDIT");
        return;
    }
    const BlockDef* def = pc.doc->block(edit->block);
    if (!def)
        return;
    auto run = [&](const char* line) {
        if (!pc.runCommand)
            return;
        if (pc.cancelCommand)
            pc.cancelCommand();
        pc.runCommand(line);
    };
    ImGui::Text("Editing block \"%s\" (%zu objects)", def->name.c_str(), edit->working.size());
    if (ImGui::Button("Save"))
        run("BSAVE");
    ImGui::SameLine();
    if (ImGui::Button("Save As..."))
        run("BSAVEAS");
    ImGui::SameLine();
    if (ImGui::Button("Test Block"))
        run("BTESTBLOCK");
    ImGui::SameLine();
    if (ImGui::Button("Close"))
        run("BCLOSE");

    ImGui::SeparatorText("Parameters");
    for (const char* k : {"Linear", "Rotation", "Flip", "Visibility", "Table"}) {
        if (ImGui::SmallButton(k)) {
            run("BPARAM");
            pc.runCommand(k);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    if (def->dynamic)
        for (std::size_t i = 0; i < def->dynamic->params.size(); ++i) {
            const DynParam& p = def->dynamic->params[i];
            std::size_t actions = 0;
            for (const DynAction& a : def->dynamic->actions)
                actions += a.param == static_cast<int>(i);
            ImGui::BulletText("%s (%s)%s", p.name.c_str(), kindName(p.kind),
                              actions ? (", " + std::to_string(actions) + " actions").c_str() : "");
        }
    if (ImGui::Button("Add Action..."))
        run("BACTION");
    ImGui::SameLine();
    if (ImGui::Button("Lookup Table..."))
        run("BLOOKUPTABLE");
    ImGui::SameLine();
    if (ImGui::Button("Attribute Order..."))
        run("BATTORDER");

    ImGui::SeparatorText("Visibility states");
    const int vi = visibilityParam(*def);
    if (vi >= 0) {
        const DynParam& p = def->dynamic->params[static_cast<std::size_t>(vi)];
        const std::string cur = currentState(*edit, p);
        for (const std::string& st : p.states)
            if (ImGui::RadioButton(st.c_str(), st == cur) && st != cur) {
                run("BVSTATE");
                pc.runCommand("Set");
                pc.runCommand(st);
            }
    } else {
        ImGui::TextDisabled("None");
    }
    if (ImGui::SmallButton("New state..."))
        run("BVSTATE");
    if (vi >= 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Hide objects"))
            run("BVHIDE");
        ImGui::SameLine();
        if (ImGui::SmallButton("Show objects"))
            run("BVSHOW");
    }
    if (!detail::testReferences(*pc.host).empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Test references are in the drawing.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Close test")) {
            run("BTESTBLOCK");
            pc.runCommand("Close");
        }
    }
}

#endif

}  // namespace

std::unique_ptr<cmd::Command> makeBSaveAsCommand() {
    return std::make_unique<BSaveAsCommand>();
}
std::unique_ptr<cmd::Command> makeBvStateCommand() {
    return std::make_unique<BvStateCommand>();
}
std::unique_ptr<cmd::Command> makeBvHideShowCommand(bool show) {
    return std::make_unique<BvHideShowCommand>(show);
}
std::unique_ptr<cmd::Command> makeBLookupTableCommand() {
    return std::make_unique<BLookupTableCommand>();
}
std::unique_ptr<cmd::Command> makeBTestBlockCommand() {
    return std::make_unique<BTestBlockCommand>();
}
std::unique_ptr<cmd::Command> makeBAttOrderCommand() {
    return std::make_unique<BAttOrderCommand>();
}

void registerBlockAuthoringPanel(ui::UiRegistry& ui) {
#ifdef TBCAD_BLOCK_PANEL_IMGUI
    ui.addPanel("block_authoring", "Block Authoring", drawAuthoring, false);
#else
    (void)ui;
#endif
}

}  // namespace tbcad::block_cmds
