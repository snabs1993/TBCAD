#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "commands.h"
#include "tbcad/layout/layout.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

// The objects of one space (model space, or the sheet of a layout) from back to front.
std::vector<EntityId> spaceOrder(const Document& doc, BlockId space) {
    if (!space)
        return doc.modelSpace();
    const BlockDef* b = doc.block(space);
    return b ? b->entities : std::vector<EntityId>{};
}

// Group names are not case sensitive (AutoCAD).
bool sameGroupName(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// ---- GROUP / UNGROUP ----------------------------------------------------------------------------

class GroupCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "GROUP"; }
    std::vector<std::string> defaultAliases() const override { return {"G"}; }
    std::string_view description() const override {
        return "Make objects that are selected together";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("GROUP");
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) { return isModifiable(doc, e); };
        so.allowPreselection = true;
        auto sel = co_await ctx.getSelection("Select objects to group", so);
        if (!sel.ok())
            co_return;
        std::vector<Group> groups = readGroups(ctx.doc());
        std::string fallback = "GROUP" + std::to_string(groups.size() + 1);
        cmd::StringOptions sopt;
        sopt.defaultValue = fallback;
        auto name = co_await ctx.getString("Enter a group name <" + fallback + ">", sopt);
        if (!name.ok())
            co_return;
        std::string chosen = cmd::trim(name.text);
        if (chosen.empty())
            chosen = fallback;
        for (const Group& g : groups) {
            if (sameGroupName(g.name, chosen)) {
                ctx.message("There is already a group named \"" + chosen + "\".");
                co_return;
            }
        }
        Group g;
        g.name = chosen;
        // Members of a group made earlier move into the new one, so a pick never selects two
        // overlapping groups.
        g.members = sel.ids;
        const std::unordered_set<EntityId> taken(sel.ids.begin(), sel.ids.end());
        for (Group& other : groups)
            std::erase_if(other.members, [&](EntityId id) { return taken.count(id) != 0; });
        groups.push_back(std::move(g));
        if (auto r = writeGroups(ctx.tx(), groups); !r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message("Group \"" + chosen + "\" made of " + std::to_string(sel.ids.size()) +
                    " objects.");
    }
};

// -GROUP: the command-line form, groups addressed by name.
class GroupLineCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-GROUP"; }
    std::vector<std::string> defaultAliases() const override { return {"-G"}; }
    std::string_view description() const override {
        return "Make, change and list groups from the command line";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("GROUP");
        cmd::KeywordOptions ko;
        ko.keywords = {"?", "Add", "Remove", "REName", "Selectable", "Create", "Explode"};
        ko.defaultKeyword = "Create";
        auto k = co_await ctx.getKeyword(
            "Enter a group option [?/Add/Remove/REName/Selectable/Create/Explode] <Create>", ko);
        if (!k.ok())
            co_return;
        std::vector<Group> groups = readGroups(ctx.doc());
        auto findNamed = [&](const std::string& n) -> Group* {
            for (Group& g : groups) {
                if (sameGroupName(g.name, n))
                    return &g;
            }
            return nullptr;
        };
        auto store = [&] {
            if (auto r = writeGroups(ctx.tx(), groups); !r)
                ctx.message(r.error().message);
        };
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) { return isModifiable(doc, e); };
        so.allowPreselection = true;

        if (k.keyword == "?") {
            if (groups.empty())
                ctx.message("No groups in this drawing.");
            for (const Group& g : groups)
                ctx.message("Group \"" + g.name + "\": " + std::to_string(g.members.size()) +
                            " objects" + (g.selectable ? "" : ", not selectable"));
            co_return;
        }
        if (k.keyword == "Create") {
            std::string fallback = "GROUP" + std::to_string(groups.size() + 1);
            auto name = co_await ctx.getString("Enter group name <" + fallback + ">",
                                               cmd::StringOptions{.defaultValue = fallback});
            if (!name.ok())
                co_return;
            std::string chosen = cmd::trim(name.text);
            if (chosen.empty())
                chosen = fallback;
            if (findNamed(chosen)) {
                ctx.message("There is already a group named \"" + chosen + "\".");
                co_return;
            }
            auto sel = co_await ctx.getSelection("Select objects for grouping", so);
            if (!sel.ok() || sel.ids.empty())
                co_return;
            const std::unordered_set<EntityId> taken(sel.ids.begin(), sel.ids.end());
            for (Group& other : groups)
                std::erase_if(other.members, [&](EntityId id) { return taken.count(id) != 0; });
            groups.push_back(Group{chosen, sel.ids, true});
            store();
            ctx.message("Group \"" + chosen + "\" made of " + std::to_string(sel.ids.size()) +
                        " objects.");
            co_return;
        }
        auto gname = co_await ctx.getString("Enter group name or [?]");
        if (!gname.ok())
            co_return;
        std::string target = cmd::trim(gname.text);
        Group* g = findNamed(target);
        if (!g) {
            ctx.message("No group named \"" + target + "\".");
            co_return;
        }
        if (k.keyword == "Explode") {
            const std::string actual = g->name;
            std::erase_if(groups, [&](const Group& x) { return x.name == actual; });
            store();
            ctx.message("Group \"" + actual + "\" dissolved.");
        } else if (k.keyword == "REName") {
            auto n = co_await ctx.getString("Enter new group name");
            if (!n.ok())
                co_return;
            std::string chosen = cmd::trim(n.text);
            if (chosen.empty())
                co_return;
            if (Group* other = findNamed(chosen); other && other != g) {
                ctx.message("There is already a group named \"" + chosen + "\".");
                co_return;
            }
            g->name = chosen;
            store();
        } else if (k.keyword == "Selectable") {
            auto a = co_await ctx.getKeyword(
                "Select the whole group on a pick [Yes/No] <Yes>",
                cmd::KeywordOptions{.keywords = {"Yes", "No"}, .defaultKeyword = "Yes"});
            if (!a.ok())
                co_return;
            g->selectable = a.keyword == "Yes";
            store();
        } else if (k.keyword == "Remove") {
            auto sel = co_await ctx.getSelection("Select objects to remove from the group", so);
            if (!sel.ok())
                co_return;
            const std::unordered_set<EntityId> out(sel.ids.begin(), sel.ids.end());
            std::erase_if(g->members, [&](EntityId id) { return out.count(id) != 0; });
            store();
        } else {  // Add
            auto sel = co_await ctx.getSelection("Select objects to add to the group", so);
            if (!sel.ok())
                co_return;
            const std::unordered_set<EntityId> picked(sel.ids.begin(), sel.ids.end());
            for (Group& other : groups) {
                if (&other != g)
                    std::erase_if(other.members,
                                  [&](EntityId id) { return picked.count(id) != 0; });
            }
            std::unordered_set<EntityId> have(g->members.begin(), g->members.end());
            for (EntityId id : sel.ids) {
                if (have.insert(id).second)
                    g->members.push_back(id);
            }
            store();
            ctx.message(std::to_string(sel.ids.size()) + " added to \"" + g->name + "\".");
        }
    }
};

class UngroupCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "UNGROUP"; }
    std::vector<std::string> defaultAliases() const override { return {"UG"}; }
    std::string_view description() const override { return "Dissolve the groups of objects"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("UNGROUP");
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) {
            for (const Group& g : readGroups(doc)) {
                if (std::find(g.members.begin(), g.members.end(), e.common.id) != g.members.end())
                    return true;
            }
            return false;
        };
        auto pick = co_await ctx.getEntity("Select a group to dissolve", eo);
        if (!pick.ok())
            co_return;
        std::vector<Group> groups = readGroups(ctx.doc());
        std::size_t removed = 0;
        std::vector<Group> kept;
        for (Group& g : groups) {
            if (std::find(g.members.begin(), g.members.end(), pick.id) != g.members.end())
                ++removed;
            else
                kept.push_back(std::move(g));
        }
        if (auto r = writeGroups(ctx.tx(), kept); !r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message(std::to_string(removed) + (removed == 1 ? " group" : " groups") +
                    " dissolved.");
    }
};

class GroupEditCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "GROUPEDIT"; }
    std::vector<std::string> defaultAliases() const override { return {"GE"}; }
    std::string_view description() const override {
        return "Add or remove group members, rename a group, or make it unselectable";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("GROUPEDIT");
        auto inGroup = [&doc = ctx.doc()](const Entity& e) {
            for (const Group& g : readGroups(doc)) {
                if (std::find(g.members.begin(), g.members.end(), e.common.id) != g.members.end())
                    return true;
            }
            return false;
        };
        cmd::EntityOptions eo;
        eo.filter = inGroup;
        auto pick = co_await ctx.getEntity("Select a group", eo);
        if (!pick.ok())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Add", "Remove", "Rename", "Selectable"};
        ko.defaultKeyword = "Add";
        auto k = co_await ctx.getKeyword("Group option [Add/Remove/Rename/Selectable] <Add>", ko);
        if (!k.ok())
            co_return;

        auto find = [&](std::vector<Group>& groups) -> Group* {
            for (Group& g : groups) {
                if (std::find(g.members.begin(), g.members.end(), pick.id) != g.members.end())
                    return &g;
            }
            return nullptr;
        };

        if (k.keyword == "Rename") {
            cmd::StringOptions so;
            auto n = co_await ctx.getString("New group name", so);
            if (!n.ok())
                co_return;
            std::string chosen = cmd::trim(n.text);
            std::vector<Group> groups = readGroups(ctx.doc());
            Group* g = find(groups);
            if (!g || chosen.empty())
                co_return;
            for (const Group& other : groups) {
                if (&other != g && sameGroupName(other.name, chosen)) {
                    ctx.message("There is already a group named \"" + chosen + "\".");
                    co_return;
                }
            }
            g->name = chosen;
            if (auto r = writeGroups(ctx.tx(), groups); !r)
                ctx.message(r.error().message);
            co_return;
        }
        if (k.keyword == "Selectable") {
            cmd::KeywordOptions yn;
            yn.keywords = {"Yes", "No"};
            yn.defaultKeyword = "Yes";
            auto a = co_await ctx.getKeyword("Select the whole group on a pick [Yes/No] <Yes>", yn);
            if (!a.ok())
                co_return;
            std::vector<Group> groups = readGroups(ctx.doc());
            Group* g = find(groups);
            if (!g)
                co_return;
            g->selectable = a.keyword == "Yes";
            if (auto r = writeGroups(ctx.tx(), groups); !r)
                ctx.message(r.error().message);
            co_return;
        }

        if (k.keyword == "Remove") {
            // One member at a time: a selection would bring the whole group along.
            std::size_t removed = 0;
            std::string target;
            {
                std::vector<Group> groups = readGroups(ctx.doc());
                if (Group* g = find(groups))
                    target = g->name;
            }
            for (;;) {
                // The group is found again by name: the picked object may be the one just removed.
                std::vector<Group> groups = readGroups(ctx.doc());
                Group* g = nullptr;
                for (Group& cand : groups) {
                    if (cand.name == target)
                        g = &cand;
                }
                if (!g)
                    break;
                cmd::EntityOptions mo;
                mo.filter = [members = g->members](const Entity& e) {
                    return std::find(members.begin(), members.end(), e.common.id) != members.end();
                };
                auto m = co_await ctx.getEntity("Select a member to remove (Enter to finish)", mo);
                if (!m.ok())
                    break;
                g->members.erase(std::remove(g->members.begin(), g->members.end(), m.id),
                                 g->members.end());
                if (auto r = writeGroups(ctx.tx(), groups); !r) {
                    ctx.message(r.error().message);
                    co_return;
                }
                ++removed;
            }
            if (removed)
                ctx.message(std::to_string(removed) + " removed from \"" + target + "\".");
            co_return;
        }

        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) { return isModifiable(doc, e); };
        auto sel = co_await ctx.getSelection("Select objects to add to the group", so);
        if (!sel.ok())
            co_return;
        std::vector<Group> groups = readGroups(ctx.doc());
        Group* g = find(groups);
        if (!g)
            co_return;
        const std::unordered_set<EntityId> picked(sel.ids.begin(), sel.ids.end());
        for (Group& other : groups) {
            if (&other == g)
                continue;
            std::erase_if(other.members, [&](EntityId id) { return picked.count(id) != 0; });
        }
        std::unordered_set<EntityId> have(g->members.begin(), g->members.end());
        for (EntityId id : sel.ids) {
            if (have.insert(id).second)
                g->members.push_back(id);
        }
        if (auto r = writeGroups(ctx.tx(), groups); !r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message(std::to_string(sel.ids.size()) + " added to \"" + g->name + "\".");
    }
};

// ---- DRAWORDER ----------------------------------------------------------------------------------

class DrawOrderCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "DRAWORDER"; }
    std::vector<std::string> defaultAliases() const override { return {"DR"}; }
    std::string_view description() const override {
        return "Change which objects are drawn in front";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.setUndoLabel("DRAWORDER");
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) { return isModifiable(doc, e); };
        auto sel = co_await ctx.getSelection("Select objects", so);
        if (!sel.ok())
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Front", "Back", "Above", "Below"};
        ko.defaultKeyword = "Front";
        auto k = co_await ctx.getKeyword(
            "Enter object ordering option [Above objects/Below objects/Front/Back] <Front>", ko);
        if (k.status == cmd::Status::Cancel)
            co_return;
        std::string mode = k.keyword.empty() ? "Front" : k.keyword;
        // Keep the objects' current relative order.
        // The space of the first selected object (model space or a layout's sheet); objects of
        // other spaces are left alone.
        const BlockId space = sel.ids.empty() ? BlockId{} : ctx.doc().ownerBlock(sel.ids.front());
        std::vector<EntityId> order = spaceOrder(ctx.doc(), space);
        std::vector<EntityId> moving;
        const std::unordered_set<EntityId> selected(sel.ids.begin(), sel.ids.end());
        for (EntityId id : order) {
            if (selected.count(id))
                moving.push_back(id);
        }
        if (moving.empty()) {
            ctx.message("None of the objects could be reordered.");
            co_return;
        }
        if (mode == "Front") {
            for (EntityId id : moving)
                (void)ctx.tx().bringToFront(id);
        } else if (mode == "Back") {
            for (auto it = moving.rbegin(); it != moving.rend(); ++it)
                (void)ctx.tx().sendToBack(*it);
        } else {
            cmd::EntityOptions eo;
            eo.filter = [&doc = ctx.doc(), &selected](const Entity& e) {
                return isModifiable(doc, e) && !selected.count(e.common.id);
            };
            auto ref = co_await ctx.getEntity("Select reference object", eo);
            if (!ref.ok())
                co_return;
            order = spaceOrder(ctx.doc(), space);
            auto at = std::find(order.begin(), order.end(), ref.id);
            if (at == order.end()) {
                ctx.message("The reference object is not in the same space.");
                co_return;
            }
            std::size_t refPos = static_cast<std::size_t>(at - order.begin());
            // New order: the moving objects directly above (or below) the reference.
            std::vector<EntityId> rest;
            const std::unordered_set<EntityId> movingSet(moving.begin(), moving.end());
            for (EntityId id : order) {
                if (!movingSet.count(id))
                    rest.push_back(id);
            }
            auto restRef = std::find(rest.begin(), rest.end(), ref.id) - rest.begin();
            (void)refPos;
            std::size_t insertAt = static_cast<std::size_t>(restRef) + (mode == "Above" ? 1 : 0);
            std::vector<EntityId> fresh(rest.begin(),
                                        rest.begin() + static_cast<std::ptrdiff_t>(insertAt));
            fresh.insert(fresh.end(), moving.begin(), moving.end());
            fresh.insert(fresh.end(), rest.begin() + static_cast<std::ptrdiff_t>(insertAt),
                         rest.end());
            // Everything from the first moved position on is re-stacked, back to front.
            std::size_t first = insertAt;
            for (std::size_t i = first; i < fresh.size(); ++i)
                (void)ctx.tx().bringToFront(fresh[i]);
        }
        ctx.message(std::to_string(moving.size()) + " objects reordered.");
    }
};

// ---- TEXTTOFRONT / HATCHTOBACK ------------------------------------------------------------------

// Puts every text, dimension or hatch of model space in front of or behind everything else,
// keeping their order among themselves.
class KindOrderCommand final : public cmd::Command {
public:
    enum class Mode { TextToFront, HatchToBack };
    explicit KindOrderCommand(Mode m) : mode_(m) {}

    std::string_view name() const override {
        return mode_ == Mode::TextToFront ? "TEXTTOFRONT" : "HATCHTOBACK";
    }
    std::string_view description() const override {
        return mode_ == Mode::TextToFront
                   ? "Bring all text and dimensions in front of other objects"
                   : "Send all hatches behind other objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string which = "Both";
        if (mode_ == Mode::TextToFront) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Text", "Dimensions", "Both"};
            ko.defaultKeyword = "Both";
            auto k = co_await ctx.getKeyword("Bring to front [Text/Dimensions/Both] <Both>", ko);
            if (!k.ok())
                co_return;
            which = k.keyword;
        }
        auto wanted = [&](const Entity& e) {
            switch (kind(e)) {
                case EntityKind::Text:
                case EntityKind::MText:
                    return mode_ == Mode::TextToFront && which != "Dimensions";
                case EntityKind::Dimension:
                case EntityKind::Leader:
                case EntityKind::MLeader:
                    return mode_ == Mode::TextToFront && which != "Text";
                case EntityKind::Hatch:
                    return mode_ == Mode::HatchToBack;
                default:
                    return false;
            }
        };
        // Model space and the sheet of every layout (a title block's text goes over its lines too).
        std::vector<BlockId> spaces{BlockId{}};
        for (const layout::Layout& l : layout::layouts(ctx.doc()))
            spaces.push_back(l.content);
        ctx.setUndoLabel(std::string(name()));
        std::size_t count = 0;
        for (BlockId space : spaces) {
            std::vector<EntityId> moving;
            for (EntityId id : spaceOrder(ctx.doc(), space)) {
                const Entity* e = ctx.doc().find(id);
                if (e && wanted(*e) && isModifiable(ctx.doc(), *e))
                    moving.push_back(id);
            }
            if (mode_ == Mode::TextToFront) {
                for (EntityId id : moving)
                    (void)ctx.tx().bringToFront(id);
            } else {
                for (auto it = moving.rbegin(); it != moving.rend(); ++it)
                    (void)ctx.tx().sendToBack(*it);
            }
            count += moving.size();
        }
        ctx.message(std::to_string(count) + " objects reordered.");
    }

private:
    Mode mode_;
};

}  // namespace

std::unique_ptr<cmd::Command> makeTextToFront() {
    return std::make_unique<KindOrderCommand>(KindOrderCommand::Mode::TextToFront);
}
std::unique_ptr<cmd::Command> makeHatchToBack() {
    return std::make_unique<KindOrderCommand>(KindOrderCommand::Mode::HatchToBack);
}

std::unique_ptr<cmd::Command> makeGroup() {
    return std::make_unique<GroupCommand>();
}
std::unique_ptr<cmd::Command> makeGroupLine() {
    return std::make_unique<GroupLineCommand>();
}
std::unique_ptr<cmd::Command> makeGroupEdit() {
    return std::make_unique<GroupEditCommand>();
}
std::unique_ptr<cmd::Command> makeUngroup() {
    return std::make_unique<UngroupCommand>();
}
std::unique_ptr<cmd::Command> makeDrawOrder() {
    return std::make_unique<DrawOrderCommand>();
}

}  // namespace tbcad::modifycmd
