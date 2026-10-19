#include <algorithm>
#include <unordered_set>

#include "commands.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

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
        std::string chosen = name.text.empty() ? fallback : name.text;
        for (const Group& g : groups) {
            if (g.name == chosen) {
                ctx.message("There is already a group named \"" + chosen + "\".");
                co_return;
            }
        }
        Group g;
        g.name = chosen;
        // Members of a group made earlier move into the new one, so a pick never selects two
        // overlapping groups.
        for (EntityId id : sel.ids) {
            g.members.push_back(id);
            for (Group& other : groups)
                other.members.erase(std::remove(other.members.begin(), other.members.end(), id),
                                    other.members.end());
        }
        groups.push_back(std::move(g));
        if (auto r = writeGroups(ctx.tx(), groups); !r) {
            ctx.message(r.error().message);
            co_return;
        }
        ctx.message("Group \"" + chosen + "\" made of " + std::to_string(sel.ids.size()) +
                    " objects.");
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
                if (&other != g && other.name == chosen) {
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
            for (;;) {
                std::vector<Group> groups = readGroups(ctx.doc());
                Group* g = find(groups);
                if (!g)
                    break;
                target = g->name;
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
        std::vector<EntityId> order = ctx.doc().modelSpace();
        std::vector<EntityId> moving;
        const std::unordered_set<EntityId> selected(sel.ids.begin(), sel.ids.end());
        for (EntityId id : order) {
            if (selected.count(id))
                moving.push_back(id);
        }
        if (moving.empty()) {
            ctx.message("None of the objects is in model space.");
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
            order = ctx.doc().modelSpace();
            auto at = std::find(order.begin(), order.end(), ref.id);
            if (at == order.end()) {
                ctx.message("The reference object is not in model space.");
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
        std::vector<EntityId> moving;
        for (EntityId id : ctx.doc().modelSpace()) {
            const Entity* e = ctx.doc().find(id);
            if (e && wanted(*e) && isModifiable(ctx.doc(), *e))
                moving.push_back(id);
        }
        ctx.setUndoLabel(std::string(name()));
        if (mode_ == Mode::TextToFront) {
            for (EntityId id : moving)
                (void)ctx.tx().bringToFront(id);
        } else {
            for (auto it = moving.rbegin(); it != moving.rend(); ++it)
                (void)ctx.tx().sendToBack(*it);
        }
        ctx.message(std::to_string(moving.size()) + " objects reordered.");
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
