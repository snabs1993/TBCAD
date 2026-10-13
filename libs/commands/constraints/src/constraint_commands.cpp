#include <cmath>
#include <set>

#include "tbcad/cmd/cmd.h"
#include "tbcad/constraints/constraints.h"
#include "tbcad/constraintscmd/constraintscmd.h"

namespace tbcad::constraintscmd {

namespace {

using namespace tbcad::constraints;

// The picked object is a line, circle, arc or point.
bool constrainable(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Point:
            return true;
        default:
            return false;
    }
}

std::vector<std::pair<Part, Vec2>> pointsOf(const Entity& e) {
    if (const auto* l = entityAs<LineData>(e))
        return {{Part::Start, l->a}, {Part::End, l->b}};
    if (const auto* c = entityAs<CircleData>(e))
        return {{Part::Center, c->center}};
    if (const auto* a = entityAs<ArcData>(e)) {
        return {{Part::Center, a->center},
                {Part::Start,
                 a->center + Vec2{std::cos(a->startAngle), std::sin(a->startAngle)} * a->radius},
                {Part::End,
                 a->center + Vec2{std::cos(a->endAngle), std::sin(a->endAngle)} * a->radius}};
    }
    if (const auto* p = entityAs<PointData>(e))
        return {{Part::Start, p->p}};
    return {};
}

// The point of the picked entity closest to where the user clicked.
Part nearestPart(const Entity& e, Vec2 pick) {
    Part best = Part::Center;
    double bestD = 1e300;
    for (const auto& [part, p] : pointsOf(e)) {
        const double d = length(p - pick);
        if (d < bestD) {
            bestD = d;
            best = part;
        }
    }
    return best;
}

std::string failText(const SolveResult& r, const Document& doc, const ConstraintSet& set) {
    std::string s = "The constraints cannot all be satisfied";
    if (const Constraint* c = set.find(r.worst))
        s += " (check " + describe(doc, *c) + ")";
    return s;
}

// Adds `c` to the set, solves, and stores both. Reports and leaves the drawing alone on failure.
bool apply(cmd::CommandContext& ctx, Constraint c) {
    if (auto ok = validate(ctx.doc(), c); !ok) {
        ctx.message(ok.error().message);
        return false;
    }
    ConstraintSet set = load(ctx.doc());
    const auto id = set.add(c);
    Transaction& tx = ctx.tx();
    auto res = solve(tx, set);
    if (!res) {
        ctx.message(res.error().message);
        return false;
    }
    if (!res.value().converged) {
        ctx.message(failText(res.value(), ctx.doc(), set));
        return false;
    }
    if (auto ok = store(tx, set); !ok) {
        ctx.message(ok.error().message);
        return false;
    }
    ctx.message(describe(ctx.doc(), *set.find(id)) + " (" +
                std::to_string(res.value().degreesOfFreedom) + " degrees of freedom left)");
    return true;
}

cmd::Task pickConstrainable(cmd::CommandContext& ctx, const char* prompt, cmd::EntityResult& out) {
    cmd::EntityOptions eo;
    eo.filter = [](const Entity& e) { return constrainable(e); };
    out = co_await ctx.getEntity(prompt, std::move(eo));
}

// GEOMCON: Coincident, Horizontal, Vertical, Parallel, Perpendicular, Equal, Tangent, Fix.
class GeomCon final : public cmd::Command {
public:
    std::string_view name() const override { return "GEOMCON"; }
    std::vector<std::string> defaultAliases() const override { return {"GCON"}; }
    std::string_view description() const override {
        return "Add a geometric constraint between objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Coincident",    "Horizontal", "Vertical", "Parallel",
                       "Perpendicular", "Equal",      "Tangent",  "Fix"};
        auto k = co_await ctx.getKeyword("Constraint type", std::move(ko));
        if (k.status != cmd::Status::Keyword)
            co_return;
        const std::string type = k.keyword;

        Constraint c;
        cmd::EntityResult first;
        co_await pickConstrainable(ctx, "Select first object", first);
        if (!first.ok())
            co_return;
        const Entity* e1 = ctx.doc().find(first.id);
        if (!e1) {
            co_return;
        }

        if (type == "Fix") {
            c.kind = Kind::Fix;
            c.a = {first.id, Part::Whole};
            (void)apply(ctx, c);
            co_return;
        }
        if (type == "Horizontal" || type == "Vertical") {
            c.kind = type == "Horizontal" ? Kind::Horizontal : Kind::Vertical;
            if (kind(*e1) == EntityKind::Line) {
                c.a = {first.id, Part::Whole};
                (void)apply(ctx, c);
                co_return;
            }
            c.a = {first.id, nearestPart(*e1, first.pickPoint)};
            cmd::EntityResult second;
            co_await pickConstrainable(ctx, "Select second point object", second);
            if (!second.ok())
                co_return;
            const Entity* e2 = ctx.doc().find(second.id);
            if (!e2)
                co_return;
            c.b = {second.id, nearestPart(*e2, second.pickPoint)};
            (void)apply(ctx, c);
            co_return;
        }

        cmd::EntityResult second;
        co_await pickConstrainable(ctx, "Select second object", second);
        if (!second.ok())
            co_return;
        const Entity* e2 = ctx.doc().find(second.id);
        if (!e2)
            co_return;
        if (type == "Coincident") {
            c.kind = Kind::Coincident;
            c.a = {first.id, nearestPart(*e1, first.pickPoint)};
            c.b = {second.id, nearestPart(*e2, second.pickPoint)};
        } else {
            c.kind = type == "Parallel"        ? Kind::Parallel
                     : type == "Perpendicular" ? Kind::Perpendicular
                     : type == "Equal"         ? Kind::Equal
                                               : Kind::Tangent;
            c.a = {first.id, Part::Whole};
            c.b = {second.id, Part::Whole};
        }
        (void)apply(ctx, c);
    }
};

// DIMCON: Distance, Length, Radius, Diameter, Angle. The default value is the current one.
class DimCon final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMCON"; }
    std::vector<std::string> defaultAliases() const override { return {"DCON"}; }
    std::string_view description() const override {
        return "Add a dimensional constraint (distance, radius, angle)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Distance", "Length", "Radius", "Diameter", "Angle"};
        auto k = co_await ctx.getKeyword("Dimensional constraint", std::move(ko));
        if (k.status != cmd::Status::Keyword)
            co_return;
        const std::string type = k.keyword;

        Constraint c;
        cmd::EntityResult first;
        if (type == "Distance") {
            co_await pickConstrainable(ctx, "Select first point object", first);
            if (!first.ok())
                co_return;
            cmd::EntityResult second;
            co_await pickConstrainable(ctx, "Select second point object", second);
            if (!second.ok())
                co_return;
            const Entity* e1 = ctx.doc().find(first.id);
            const Entity* e2 = ctx.doc().find(second.id);
            if (!e1 || !e2)
                co_return;
            c.kind = Kind::Distance;
            c.a = {first.id, nearestPart(*e1, first.pickPoint)};
            c.b = {second.id, nearestPart(*e2, second.pickPoint)};
        } else if (type == "Length") {
            co_await pickConstrainable(ctx, "Select line", first);
            if (!first.ok())
                co_return;
            c.kind = Kind::Distance;
            c.a = {first.id, Part::Start};
            c.b = {first.id, Part::End};
        } else if (type == "Radius" || type == "Diameter") {
            co_await pickConstrainable(ctx, "Select circle or arc", first);
            if (!first.ok())
                co_return;
            c.kind = Kind::Radius;
            c.a = {first.id, Part::Whole};
        } else {
            co_await pickConstrainable(ctx, "Select first line", first);
            if (!first.ok())
                co_return;
            cmd::EntityResult second;
            co_await pickConstrainable(ctx, "Select second line", second);
            if (!second.ok())
                co_return;
            c.kind = Kind::Angle;
            c.a = {first.id, Part::Whole};
            c.b = {second.id, Part::Whole};
        }

        // Validate with a placeholder value so the user is not asked about nonsense.
        const double scale = type == "Diameter" ? 2.0 : 1.0;
        Constraint probe = c;
        probe.value = c.kind == Kind::Radius ? 1 : 0;
        if (auto ok = validate(ctx.doc(), probe); !ok) {
            ctx.message(ok.error().message);
            co_return;
        }
        auto now = measure(ctx.doc(), c);
        if (!now) {
            ctx.message(now.error().message);
            co_return;
        }
        if (c.kind == Kind::Angle) {
            cmd::AngleOptions ao;
            ao.defaultValue = now.value();
            auto v = co_await ctx.getAngle("Angle", std::move(ao));
            if (!v.ok())
                co_return;
            c.value = v.value;
        } else {
            cmd::DistanceOptions dopt;
            dopt.defaultValue = now.value() * scale;
            dopt.allowZero = c.kind == Kind::Distance;
            auto v = co_await ctx.getDistance(type == "Diameter" ? "Diameter" : "Value",
                                              std::move(dopt));
            if (!v.ok())
                co_return;
            c.value = v.value / scale;
        }
        (void)apply(ctx, c);
    }
};

class ShowCon final : public cmd::Command {
public:
    std::string_view name() const override { return "SHOWCON"; }
    std::string_view description() const override { return "List the constraints of the drawing"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const ConstraintSet set = load(ctx.doc());
        if (set.empty()) {
            ctx.message("No constraints.");
            co_return;
        }
        for (const Constraint& c : set.all())
            ctx.message(describe(ctx.doc(), c));
    }
};

class DelCon final : public cmd::Command {
public:
    std::string_view name() const override { return "DELCON"; }
    std::string_view description() const override {
        return "Remove all constraints of the selected objects";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects");
        if (sel.ids.empty())
            co_return;
        ConstraintSet set = load(ctx.doc());
        const std::set<EntityId> picked(sel.ids.begin(), sel.ids.end());
        std::size_t removed = 0;
        for (const Constraint& c : std::vector<Constraint>(set.all())) {
            if (picked.count(c.a.entity) || (c.b.entity && picked.count(c.b.entity))) {
                set.remove(c.id);
                ++removed;
            }
        }
        if (removed == 0) {
            ctx.message("The selected objects have no constraints.");
            co_return;
        }
        if (auto ok = store(ctx.tx(), set); !ok) {
            ctx.message(ok.error().message);
            co_return;
        }
        ctx.message(std::to_string(removed) + " constraint(s) removed.");
    }
};

class SolveCon final : public cmd::Command {
public:
    std::string_view name() const override { return "SOLVECON"; }
    std::string_view description() const override { return "Solve the constraints again"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const ConstraintSet set = load(ctx.doc());
        auto res = solve(ctx.tx(), set);
        if (!res) {
            ctx.message(res.error().message);
            co_return;
        }
        if (!res.value().converged) {
            ctx.message(failText(res.value(), ctx.doc(), set));
            co_return;
        }
        ctx.message("Solved: " + std::to_string(res.value().changedEntities) +
                    " object(s) moved, " + std::to_string(res.value().degreesOfFreedom) +
                    " degrees of freedom left.");
    }
};

// Keeps constrained geometry in step with edits: the objects the command touched stay as the
// user left them and the rest follows. If the constraints then have no solution the edit stands
// as it is and the other objects stay where they were (SOLVECON reports what is in conflict).
void keepInStep(Document& doc, Transaction& tx) {
    auto it = doc.header().custom.find(kHeaderKey);
    if (it == doc.header().custom.end())
        return;
    ConstraintSet set = ConstraintSet::fromText(it->second);
    if (set.empty())
        return;

    // Forget constraints whose objects were erased.
    const std::size_t gone = set.removeIf([&](EntityId e) { return !doc.contains(e); });
    if (gone > 0)
        (void)store(tx, set);
    if (set.empty())
        return;

    std::set<EntityId> pinned;
    for (EntityId id : tx.touchedEntities()) {
        if (set.involves(id))
            pinned.insert(id);
    }
    if (pinned.empty())
        return;
    SolveOptions opt;
    opt.pinned = pinned;
    (void)solve(tx, set, opt);
}

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<GeomCon>());
    (void)registry.add(std::make_unique<DimCon>());
    (void)registry.add(std::make_unique<ShowCon>());
    (void)registry.add(std::make_unique<DelCon>());
    (void)registry.add(std::make_unique<SolveCon>());
    registry.addCommitHook(keepInStep);
}

}  // namespace tbcad::constraintscmd
