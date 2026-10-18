#include <map>

#include "commands.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

bool unlocked(const Document& doc, const Entity& e) {
    const EffectiveLayer l = doc.effective(e.common);
    return !l.locked && !l.otherLevel;
}

// What the last ERASE removed, per drawing (by uid: addresses get reused), so OOPS can bring it
// back even after other commands.
std::map<std::uint64_t, std::vector<Entity>>& lastErased() {
    static std::map<std::uint64_t, std::vector<Entity>> store;
    return store;
}

class BreakCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BREAK"; }
    std::vector<std::string> defaultAliases() const override { return {"BR"}; }
    std::string_view description() const override {
        return "Remove the part of an object between two points";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return isEditable(doc, e); };
        auto pick = co_await ctx.getEntity("Select object to break", eo);
        if (!pick.ok())
            co_return;
        Vec2 p1 = pick.pickPoint;
        Vec2 p2{};
        for (;;) {
            cmd::PointOptions po;
            po.keywords = {"First point"};
            po.base = p1;
            auto r = co_await ctx.getPoint("Specify second break point or [First point]", po);
            if (r.status == cmd::Status::Keyword) {
                auto f = co_await ctx.getPoint("Specify first break point");
                if (f.ok())
                    p1 = f.point;
                continue;
            }
            if (!r.ok())
                co_return;
            p2 = r.point;
            break;
        }
        const Entity* e = ctx.doc().find(pick.id);
        if (!e)
            co_return;
        auto edit = modify::samePoint(p1, p2) ? modify::breakAtPoint(*e, p1)
                                              : modify::breakEntity(*e, p1, p2);
        if (!edit) {
            ctx.message(edit.error().message);
            co_return;
        }
        if (auto ids = applyEdit(ctx.tx(), pick.id, std::move(edit.value())); !ids)
            ctx.message(ids.error().message);
    }
};

class BreakAtPointCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "BREAKATPOINT"; }
    std::vector<std::string> defaultAliases() const override { return {"BAP"}; }
    std::string_view description() const override { return "Split an object in two at a point"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::EntityOptions eo;
        eo.filter = [&doc = ctx.doc()](const Entity& e) { return isEditable(doc, e); };
        auto pick = co_await ctx.getEntity("Select object to break", eo);
        if (!pick.ok())
            co_return;
        auto p = co_await ctx.getPoint("Specify break point");
        if (!p.ok())
            co_return;
        const Entity* e = ctx.doc().find(pick.id);
        if (!e)
            co_return;
        auto edit = modify::breakAtPoint(*e, p.point);
        if (!edit) {
            ctx.message(edit.error().message);
            co_return;
        }
        if (auto ids = applyEdit(ctx.tx(), pick.id, std::move(edit.value())); !ids)
            ctx.message(ids.error().message);
    }
};

class JoinCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "JOIN"; }
    std::vector<std::string> defaultAliases() const override { return {"J"}; }
    std::string_view description() const override {
        return "Join lines, arcs and polylines that touch or overlap";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) {
            if (!isEditable(doc, e))
                return false;
            return kind(e) == EntityKind::Line || kind(e) == EntityKind::Arc ||
                   kind(e) == EntityKind::Polyline;
        };
        auto sel = co_await ctx.getSelection("Select objects to join", so);
        if (!sel.ok())
            co_return;
        std::vector<EntityId> ids;
        std::vector<const Entity*> entities;
        for (EntityId id : sel.ids) {
            if (const Entity* e = ctx.doc().find(id)) {
                ids.push_back(id);
                entities.push_back(e);
            }
        }
        // Like AutoCAD, collinear lines and arcs of one circle join across a gap.
        auto res = modify::joinEntities(entities, getSetting(ctx.host(), "join.tolerance", 1e-3),
                                        /*bridgeGaps=*/true);
        if (res.consumed.empty()) {
            ctx.message("Nothing could be joined.");
            co_return;
        }
        // Copy the results before the erase invalidates the pointers they were built from.
        for (std::size_t i : res.consumed) {
            if (auto r = ctx.tx().erase(ids[i]); !r)
                ctx.message(r.error().message);
        }
        for (Entity& e : res.created) {
            if (auto r = ctx.tx().add(std::move(e)); !r)
                ctx.message(r.error().message);
        }
        ctx.message(std::to_string(res.consumed.size()) + " objects joined into " +
                    std::to_string(res.created.size()) + ".");
    }
};

class EraseCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ERASE"; }
    std::vector<std::string> defaultAliases() const override { return {"E"}; }
    std::string_view description() const override { return "Delete objects"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc()](const Entity& e) { return unlocked(doc, e); };
        auto sel = co_await ctx.getSelection("Select objects to erase", so);
        if (!sel.ok())
            co_return;
        std::size_t n = 0;
        std::vector<Entity> removed;
        // OOPS keeps a copy of what was erased; for a drawing-sized erase that copy would cost
        // as much as the erase (UNDO still takes such an erase back).
        constexpr std::size_t kOopsMax = 20000;
        const bool keep = sel.ids.size() <= kOopsMax;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            Entity copy;
            if (e && keep)
                copy = *e;
            if (ctx.tx().erase(id)) {
                ++n;
                if (e && keep)
                    removed.push_back(std::move(copy));
            }
        }
        if (!keep)
            lastErased().erase(ctx.doc().uid());
        if (!removed.empty())
            lastErased()[ctx.doc().uid()] = std::move(removed);
        ctx.host().setSelection({});
        ctx.message(std::to_string(n) + " erased.");
    }
};

// OOPS: puts back the objects the last ERASE removed.
class OopsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "OOPS"; }
    std::vector<std::string> defaultAliases() const override { return {}; }
    std::string_view description() const override { return "Restore the objects last erased"; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        auto it = lastErased().find(ctx.doc().uid());
        if (it == lastErased().end() || it->second.empty()) {
            ctx.message("Nothing has been erased.");
            co_return;
        }
        std::vector<EntityId> back;
        for (Entity e : it->second) {
            e.common.id = {};
            if (!ctx.doc().layer(e.common.layer))
                e.common.layer = ctx.doc().header().currentLayer;
            auto added = ctx.tx().add(std::move(e));
            if (added)
                back.push_back(added.value());
            else
                ctx.message(added.error().message);
        }
        lastErased().erase(it);
        if (back.empty())
            co_return;
        ctx.host().setSelection(back);
        ctx.setUndoLabel("OOPS");
        ctx.message(std::to_string(back.size()) + " restored.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeBreak() {
    return std::make_unique<BreakCommand>();
}
std::unique_ptr<cmd::Command> makeBreakAtPoint() {
    return std::make_unique<BreakAtPointCommand>();
}
std::unique_ptr<cmd::Command> makeJoin() {
    return std::make_unique<JoinCommand>();
}
std::unique_ptr<cmd::Command> makeErase() {
    return std::make_unique<EraseCommand>();
}
std::unique_ptr<cmd::Command> makeOops() {
    return std::make_unique<OopsCommand>();
}

}  // namespace tbcad::modifycmd
