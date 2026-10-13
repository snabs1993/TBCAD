#include "commands.h"
#include "util.h"

namespace tbcad::modifycmd {

namespace {

bool unlocked(const Document& doc, const Entity& e) {
    const EffectiveLayer l = doc.effective(e.common);
    return !l.locked && !l.otherLevel;
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
        auto res = modify::joinEntities(entities, getSetting(ctx.host(), "join.tolerance", 1e-3));
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
        for (EntityId id : sel.ids) {
            if (ctx.tx().erase(id))
                ++n;
        }
        ctx.host().setSelection({});
        ctx.message(std::to_string(n) + " erased.");
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

}  // namespace tbcad::modifycmd
