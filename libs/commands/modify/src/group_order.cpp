#include <algorithm>

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
        for (EntityId id : order) {
            if (std::find(sel.ids.begin(), sel.ids.end(), id) != sel.ids.end())
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
            eo.filter = [&doc = ctx.doc(), &sel](const Entity& e) {
                return isModifiable(doc, e) &&
                       std::find(sel.ids.begin(), sel.ids.end(), e.common.id) == sel.ids.end();
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
            for (EntityId id : order) {
                if (std::find(moving.begin(), moving.end(), id) == moving.end())
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

}  // namespace

std::unique_ptr<cmd::Command> makeGroup() {
    return std::make_unique<GroupCommand>();
}
std::unique_ptr<cmd::Command> makeUngroup() {
    return std::make_unique<UngroupCommand>();
}
std::unique_ptr<cmd::Command> makeDrawOrder() {
    return std::make_unique<DrawOrderCommand>();
}

}  // namespace tbcad::modifycmd
