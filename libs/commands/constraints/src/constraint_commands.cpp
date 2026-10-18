#include <algorithm>
#include <cmath>
#include <set>

#include "tbcad/cmd/cmd.h"
#include "tbcad/constraints/constraints.h"
#include "tbcad/constraintscmd/constraintscmd.h"
#include "tbcad/expr/expr.h"

namespace tbcad::constraintscmd {

namespace {

using namespace tbcad::constraints;

// The picked object is a line, circle, arc, point, ellipse, insert or a polyline with a straight
// segment.
bool constrainable(const Entity& e) {
    switch (kind(e)) {
        case EntityKind::Line:
        case EntityKind::Circle:
        case EntityKind::Arc:
        case EntityKind::Point:
        case EntityKind::Ellipse:
        case EntityKind::Insert:
        case EntityKind::Polyline:
            return true;
        default:
            return false;
    }
}

struct PartPoint {
    Part part;
    std::uint32_t index;
    Vec2 p;
};

std::vector<PartPoint> pointsOf(const Entity& e) {
    if (const auto* l = entityAs<LineData>(e))
        return {{Part::Start, 0, l->a}, {Part::End, 0, l->b}};
    if (const auto* c = entityAs<CircleData>(e))
        return {{Part::Center, 0, c->center}};
    if (const auto* a = entityAs<ArcData>(e)) {
        return {{Part::Center, 0, a->center},
                {Part::Start, 0,
                 a->center + Vec2{std::cos(a->startAngle), std::sin(a->startAngle)} * a->radius},
                {Part::End, 0,
                 a->center + Vec2{std::cos(a->endAngle), std::sin(a->endAngle)} * a->radius}};
    }
    if (const auto* p = entityAs<PointData>(e))
        return {{Part::Start, 0, p->p}};
    if (const auto* el = entityAs<EllipseData>(e))
        return {{Part::Center, 0, el->center}};
    if (const auto* in = entityAs<InsertData>(e))
        return {{Part::Start, 0, in->position}};
    std::vector<PartPoint> out;
    if (const auto* pl = entityAs<PolylineData>(e)) {
        for (std::size_t i = 0; i < pl->v.size(); ++i)
            out.push_back({Part::Start, static_cast<std::uint32_t>(i), pl->v[i].p});
    }
    return out;
}

// The point of the picked entity closest to where the user clicked.
Ref nearestPoint(EntityId id, const Entity& e, Vec2 pick) {
    Ref best{id, Part::Center};
    double bestD = 1e300;
    for (const PartPoint& pp : pointsOf(e)) {
        const double d = length(pp.p - pick);
        if (d < bestD) {
            bestD = d;
            best = {id, pp.part, pp.index};
        }
    }
    return best;
}

// The whole object, or for a polyline the straight segment closest to the click.
Ref wholeRef(EntityId id, const Entity& e, Vec2 pick) {
    Ref r{id, Part::Whole};
    const auto* pl = entityAs<PolylineData>(e);
    if (!pl || pl->v.size() < 2)
        return r;
    double bestD = 1e300;
    const std::size_t n = pl->v.size(), segs = pl->closed ? n : n - 1;
    for (std::size_t i = 0; i < segs; ++i) {
        if (pl->v[i].bulge != 0)
            continue;
        const Vec2 a = pl->v[i].p, b = pl->v[(i + 1) % n].p;
        const Vec2 d = b - a;
        const double len2 = dot(d, d);
        const double t = len2 > 0 ? std::clamp(dot(pick - a, d) / len2, 0.0, 1.0) : 0.0;
        const double dist = length(pick - (a + d * t));
        if (dist < bestD) {
            bestD = dist;
            r.index = static_cast<std::uint32_t>(i);
        }
    }
    return r;
}

// Does the entity act as a direction (line, polyline segment, insert)?
bool isDirection(const Entity& e) {
    const EntityKind k = kind(e);
    return k == EntityKind::Line || k == EntityKind::Polyline || k == EntityKind::Insert;
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

// Asks for a formula over the drawing's parameters and sets the constraint's expression and value.
// `scale` is the diameter factor (the formula gives the diameter; the constraint holds the radius).
cmd::Task askFormula(cmd::CommandContext& ctx, Constraint& c, double scale, bool& ok) {
    ok = false;
    const ParameterSet params = loadParameters(ctx.doc());
    auto text = co_await ctx.getString(params.empty() ? "Formula (no parameters defined yet)"
                                                      : "Formula using the parameters");
    if (!text.ok())
        co_return;
    const std::string src = cmd::trim(text.text);
    auto v = params.evaluate(src);
    if (!v) {
        ctx.message(v.error().message);
        co_return;
    }
    c.expr = src;
    c.value = c.kind == Kind::Angle ? v.value() * kPi / 180.0 : v.value() / scale;
    ok = true;
}

// GEOMCON: Coincident, Horizontal, Vertical, Parallel, Perpendicular, Equal, Tangent, Fix.
class GeomCon final : public cmd::Command {
public:
    std::string_view name() const override { return "GEOMCON"; }
    std::vector<std::string> defaultAliases() const override { return {"GCON", "GEOMCONSTRAINT"}; }
    std::string_view description() const override {
        return "Add a geometric constraint between objects";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Coincident",    "Horizontal", "Vertical", "Parallel",
                       "Perpendicular", "Equal",      "Tangent",  "Fix",
                       "Concentric",    "PointOn",    "Midpoint", "Collinear"};
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
            if (isDirection(*e1)) {
                c.a = wholeRef(first.id, *e1, first.pickPoint);
                (void)apply(ctx, c);
                co_return;
            }
            c.a = nearestPoint(first.id, *e1, first.pickPoint);
            cmd::EntityResult second;
            co_await pickConstrainable(ctx, "Select second point object", second);
            if (!second.ok())
                co_return;
            const Entity* e2 = ctx.doc().find(second.id);
            if (!e2)
                co_return;
            c.b = nearestPoint(second.id, *e2, second.pickPoint);
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
            c.a = nearestPoint(first.id, *e1, first.pickPoint);
            c.b = nearestPoint(second.id, *e2, second.pickPoint);
        } else if (type == "PointOn" || type == "Midpoint") {
            // the first pick is the point, the second the line, circle or arc it sits on
            c.kind = type == "PointOn" ? Kind::PointOnObject : Kind::Midpoint;
            c.a = nearestPoint(first.id, *e1, first.pickPoint);
            c.b = wholeRef(second.id, *e2, second.pickPoint);
        } else {
            c.kind = type == "Parallel"        ? Kind::Parallel
                     : type == "Concentric"    ? Kind::Concentric
                     : type == "Collinear"     ? Kind::Collinear
                     : type == "Perpendicular" ? Kind::Perpendicular
                     : type == "Equal"         ? Kind::Equal
                                               : Kind::Tangent;
            c.a = wholeRef(first.id, *e1, first.pickPoint);
            c.b = wholeRef(second.id, *e2, second.pickPoint);
        }
        (void)apply(ctx, c);
    }
};

// DIMCON: Distance, Length, Radius, Diameter, Angle. The default value is the current one.
class DimCon final : public cmd::Command {
public:
    std::string_view name() const override { return "DIMCON"; }
    std::vector<std::string> defaultAliases() const override { return {"DCON", "DIMCONSTRAINT"}; }
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
            c.a = nearestPoint(first.id, *e1, first.pickPoint);
            c.b = nearestPoint(second.id, *e2, second.pickPoint);
        } else if (type == "Length") {
            co_await pickConstrainable(ctx, "Select line", first);
            if (!first.ok())
                co_return;
            c.kind = Kind::Distance;
            const Entity* line = ctx.doc().find(first.id);
            if (!line)
                co_return;
            const Ref seg = wholeRef(first.id, *line, first.pickPoint);
            if (const auto* pl = entityAs<PolylineData>(*line)) {
                c.a = {first.id, Part::Start, seg.index};
                c.b = {first.id, Part::Start,
                       static_cast<std::uint32_t>((seg.index + 1) % pl->v.size())};
            } else {
                c.a = {first.id, Part::Start};
                c.b = {first.id, Part::End};
            }
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
            const Entity* l1 = ctx.doc().find(first.id);
            const Entity* l2 = ctx.doc().find(second.id);
            if (!l1 || !l2)
                co_return;
            c.kind = Kind::Angle;
            c.a = wholeRef(first.id, *l1, first.pickPoint);
            c.b = wholeRef(second.id, *l2, second.pickPoint);
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
            ao.keywords = {"Formula"};
            auto v = co_await ctx.getAngle("Angle or [Formula]", std::move(ao));
            if (v.status == cmd::Status::Keyword) {
                bool got = false;
                co_await askFormula(ctx, c, 1.0, got);
                if (!got)
                    co_return;
            } else if (!v.ok()) {
                co_return;
            } else {
                c.value = v.value;
            }
        } else {
            cmd::DistanceOptions dopt;
            dopt.defaultValue = now.value() * scale;
            dopt.allowZero = c.kind == Kind::Distance;
            dopt.keywords = {"Formula"};
            auto v = co_await ctx.getDistance(
                type == "Diameter" ? "Diameter or [Formula]" : "Value or [Formula]",
                std::move(dopt));
            if (v.status == cmd::Status::Keyword) {
                bool got = false;
                co_await askFormula(ctx, c, scale, got);
                if (!got)
                    co_return;
            } else if (!v.ok()) {
                co_return;
            } else {
                c.value = v.value / scale;
            }
        }
        (void)apply(ctx, c);
    }
};

// AUTOCONSTRAIN: adds every constraint the selected objects already satisfy.
class AutoConstrainCmd final : public cmd::Command {
public:
    std::string_view name() const override { return "AUTOCONSTRAIN"; }
    std::vector<std::string> defaultAliases() const override { return {"AUTOCON"}; }
    std::string_view description() const override {
        return "Add the constraints the selected objects already satisfy";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::SelectionOptions so;
        so.filter = [](const Entity& e) { return constrainable(e); };
        auto sel = co_await ctx.getSelection("Select objects to constrain", so);
        if (!sel.ok() || sel.ids.empty())
            co_return;
        ConstraintSet set = load(ctx.doc());
        const std::vector<Constraint> found = autoConstrain(ctx.doc(), sel.ids, set);
        if (found.empty()) {
            ctx.message("No new constraints found.");
            co_return;
        }
        std::size_t added = 0, refused = 0;
        for (const Constraint& c : found) {
            if (!validate(ctx.doc(), c)) {
                ++refused;
                continue;
            }
            ConstraintSet trial = set;
            trial.add(c);
            auto res = solve(ctx.tx(), trial);
            if (res && res.value().converged) {
                set = std::move(trial);
                ++added;
            } else {
                ++refused;
            }
        }
        if (added == 0) {
            ctx.message("None of the " + std::to_string(found.size()) +
                        " candidate constraints could be added.");
            co_return;
        }
        if (auto ok = store(ctx.tx(), set); !ok) {
            ctx.message(ok.error().message);
            co_return;
        }
        std::string msg = std::to_string(added) + " constraint(s) added";
        if (refused)
            msg += ", " + std::to_string(refused) + " skipped because they conflict";
        ctx.message(msg + ".");
    }
};

// PARAMETERS: the named values dimensional constraints can use.
class ParametersCmd final : public cmd::Command {
public:
    std::string_view name() const override { return "PARAMETERS"; }
    std::vector<std::string> defaultAliases() const override { return {"PARAM"}; }
    std::string_view description() const override {
        return "Define named parameters that drive dimensional constraints";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        ctx.commitOnCancel();
        for (;;) {
            cmd::KeywordOptions ko;
            ko.keywords = {"Set", "Delete", "List", "Exit"};
            ko.defaultKeyword = "Exit";
            auto k = co_await ctx.getKeyword("Parameters [Set/Delete/List/Exit] <Exit>", ko);
            if (!k.ok() || k.keyword == "Exit")
                co_return;
            ParameterSet params = loadParameters(ctx.doc());
            if (k.keyword == "List") {
                if (params.empty()) {
                    ctx.message("No parameters.");
                    continue;
                }
                auto vals = params.values();
                for (const Parameter& p : params.all()) {
                    std::string line = p.name + " = " + p.expr;
                    if (vals)
                        line +=
                            "  (" + expr::Value::ofNumber(vals.value().at(p.name)).toString() + ")";
                    ctx.message(line);
                }
                continue;
            }
            auto name = co_await ctx.getString("Parameter name");
            if (!name.ok())
                co_return;
            const std::string n = cmd::trim(name.text);
            ParameterSet next = params;
            if (k.keyword == "Delete") {
                if (!next.remove(n)) {
                    ctx.message("No parameter named " + n + ".");
                    continue;
                }
                const ConstraintSet cs = load(ctx.doc());
                bool used = false;
                for (const Constraint& c : cs.all())
                    if (!c.expr.empty() && !next.unknownNames(c.expr).empty())
                        used = true;
                if (!next.values())
                    used = true;  // another parameter still refers to it
                if (used) {
                    ctx.message("Constraints still use " + n + ".");
                    continue;
                }
                if (auto ok = storeParameters(ctx.tx(), next); !ok)
                    ctx.message(ok.error().message);
                continue;
            }
            const Parameter* old = params.find(n);
            auto formula = co_await ctx.getString(old ? "Formula for " + n + " <" + old->expr + ">"
                                                      : "Formula for " + n);
            if (!formula.ok())
                co_return;
            std::string src = cmd::trim(formula.text);
            if (src.empty() && old)
                src = old->expr;
            if (auto ok = next.set(n, src); !ok) {
                ctx.message(ok.error().message);
                continue;
            }
            ConstraintSet cs = load(ctx.doc());
            auto res = applyParameters(ctx.tx(), cs, next);
            if (!res) {
                ctx.message(res.error().message);
                continue;
            }
            if (!res.value().converged) {
                ctx.message("The drawing cannot follow that value" + std::string(".") + " " +
                            failText(res.value(), ctx.doc(), cs));
                continue;
            }
            if (auto ok = storeParameters(ctx.tx(), next); !ok) {
                ctx.message(ok.error().message);
                continue;
            }
            auto vals = next.values();
            ctx.message(n + " = " +
                        (vals ? expr::Value::ofNumber(vals.value().at(n)).toString() : src));
        }
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
    (void)registry.add(std::make_unique<AutoConstrainCmd>());
    (void)registry.add(std::make_unique<ParametersCmd>());
    (void)registry.add(std::make_unique<ShowCon>());
    (void)registry.add(std::make_unique<DelCon>());
    (void)registry.add(std::make_unique<SolveCon>());
    registry.addCommitHook(keepInStep);
}

}  // namespace tbcad::constraintscmd
