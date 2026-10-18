// Parametric constraints (libs/constraints) as tools: list, add, remove, solve.

#include <cmath>

#include "tbcad/constraints/constraints.h"
#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

using namespace tbcad::constraints;

void commit(DrawingSession& s, Transaction& tx, const std::string& tool) {
    ChangeSet cs = tx.commit();
    s.undoStack().push(std::string(kAiLabelPrefix) + tool, {std::move(cs)});
}

const char* partName(Part p) {
    switch (p) {
        case Part::Start:
            return "start";
        case Part::End:
            return "end";
        case Part::Center:
            return "center";
        default:
            return "whole";
    }
}

Json refJson(const Ref& r) {
    Json::Object o{{"id", idJson(r.entity)}, {"part", partName(r.part)}};
    if (r.index)
        o.emplace("index", static_cast<double>(r.index));
    return Json(std::move(o));
}

constexpr double kPiLocal = 3.14159265358979323846;

Json constraintJson(const Document& doc, const Constraint& c) {
    Json::Object o{{"id", static_cast<double>(c.id)},
                   {"kind", std::string(kindName(c.kind))},
                   {"a", refJson(c.a)},
                   {"description", describe(doc, c)}};
    if (c.b.entity)
        o["b"] = refJson(c.b);
    if (isDimensional(c.kind)) {
        if (c.kind == Kind::Angle) {
            o["value"] = c.value * 180.0 / kPiLocal;
            o["unit"] = "degrees";
        } else {
            o["value"] = c.value;
            o["unit"] = "mm";
        }
    }
    if (c.kind == Kind::DistanceRange || c.kind == Kind::AngleRange) {
        const bool deg = c.kind == Kind::AngleRange;
        o["min"] = deg ? c.value * 180.0 / kPiLocal : c.value;
        o["max"] = deg ? c.value2 * 180.0 / kPiLocal : c.value2;
        o["unit"] = deg ? "degrees" : "mm";
    }
    return Json(std::move(o));
}

Result<Ref> refFromJson(const Json& j) {
    const Json* idj = j.isObject() ? j.find("id") : &j;
    std::optional<EntityId> id = idj ? parseId(*idj) : std::nullopt;
    if (!id)
        return makeError(ErrorCode::InvalidArgument, "a reference needs an entity id");
    Ref r;
    r.entity = *id;
    std::string part = j.isObject() ? argString(j, "part", "whole") : "whole";
    if (part == "whole")
        r.part = Part::Whole;
    else if (part == "start")
        r.part = Part::Start;
    else if (part == "end")
        r.part = Part::End;
    else if (part == "center")
        r.part = Part::Center;
    else
        return makeError(ErrorCode::InvalidArgument,
                         "part must be whole, start, end or center, not \"" + part + "\"");
    // polylines: the vertex (part start) or segment (part whole), counted from 0
    const double index = j.isObject() ? argNumber(j, "index", 0) : 0;
    if (!(index >= 0) || index > 1e6)
        return makeError(ErrorCode::InvalidArgument, "index must be zero or more");
    r.index = static_cast<std::uint32_t>(index);
    return r;
}

std::optional<Kind> kindFromName(std::string name) {
    for (char& c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 0; i <= static_cast<int>(Kind::AngleRange); ++i) {
        Kind k = static_cast<Kind>(i);
        std::string n(kindName(k));
        for (char& c : n)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n == name)
            return k;
    }
    return std::nullopt;
}

std::string failText(const SolveResult& r, const Document& doc, const ConstraintSet& set) {
    std::string s =
        "the constraints cannot all be satisfied (largest error " + formatDouble(r.residual) + ")";
    if (const Constraint* c = set.find(r.worst))
        s += "; check " + describe(doc, *c);
    return s;
}

ToolResult listConstraints(ToolContext& ctx, const Json&) {
    Document& doc = ctx.session.document();
    ConstraintSet set = load(doc);
    Json::Array all;
    for (const Constraint& c : set.all())
        all.push_back(constraintJson(doc, c));
    return ToolResult::ok(Json(Json::Object{{"constraints", Json(std::move(all))}}));
}

ToolResult analyzeConstraints(ToolContext& ctx, const Json&) {
    Document& doc = ctx.session.document();
    auto an = analyze(doc, load(doc));
    if (!an)
        return ToolResult::fail(an.error().message);
    const Analysis& a = an.value();
    Json::Array cons, ents;
    for (const ConstraintStatus& c : a.constraints)
        cons.push_back(Json(Json::Object{{"id", static_cast<double>(c.id)},
                                         {"redundant", c.redundant},
                                         {"conflicting", c.conflicting}}));
    for (const EntityStatus& e : a.entities)
        ents.push_back(
            Json(Json::Object{{"id", idJson(e.entity)},
                              {"degreesOfFreedom", static_cast<double>(e.degreesOfFreedom)},
                              {"constraints", static_cast<double>(e.constraints)}}));
    return ToolResult::ok(
        Json(Json::Object{{"summary", a.summary()},
                          {"solved", a.converged},
                          {"fullyConstrained", a.fullyConstrained()},
                          {"overConstrained", a.overConstrained()},
                          {"degreesOfFreedom", static_cast<double>(a.degreesOfFreedom)},
                          {"constraints", Json(std::move(cons))},
                          {"objects", Json(std::move(ents))}}));
}

ToolResult addConstraint(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string kname = argString(a, "kind");
    auto kind = kindFromName(kname);
    if (!kind)
        return ToolResult::fail("unknown constraint kind \"" + kname + "\"");
    Constraint c;
    c.kind = *kind;
    const Json* aj = a.find("a");
    if (!aj)
        return ToolResult::fail("a is required");
    auto ra = refFromJson(*aj);
    if (!ra)
        return ToolResult::fail("a: " + ra.error().message);
    c.a = ra.value();
    if (const Json* bj = a.find("b")) {
        auto rb = refFromJson(*bj);
        if (!rb)
            return ToolResult::fail("b: " + rb.error().message);
        c.b = rb.value();
    }
    if (isDimensional(c.kind)) {
        const Json* v = a.find("value");
        if (!v || !v->isNumber())
            return ToolResult::fail("value is required for " + kname + " (mm; degrees for angle)");
        c.value = c.kind == Kind::Angle ? v->asNumber() * kPiLocal / 180.0 : v->asNumber();
    }
    if (c.kind == Kind::DistanceRange || c.kind == Kind::AngleRange) {
        const Json* lo = a.find("value");
        const Json* hi = a.find("value2");
        if (!lo || !lo->isNumber() || !hi || !hi->isNumber())
            return ToolResult::fail(
                "value (lower limit) and value2 (upper limit) are required "
                "for " +
                kname + " (mm; degrees for angles)");
        const double f = c.kind == Kind::AngleRange ? kPiLocal / 180.0 : 1.0;
        c.value = lo->asNumber() * f;
        c.value2 = hi->asNumber() * f;
    }
    if (auto ok = validate(doc, c); !ok)
        return ToolResult::fail(ok.error().message);
    if (std::string why = whyReadOnly(doc, *doc.find(c.a.entity)); !why.empty())
        return ToolResult::fail(why);

    ConstraintSet set = load(doc);
    const auto newId = set.add(c);
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "add_constraint");
    auto res = solve(tx, set);
    if (!res)
        return ToolResult::fail(res.error().message);
    if (!res.value().converged)
        return ToolResult::fail(failText(res.value(), doc, set));
    if (auto ok = store(tx, set); !ok)
        return ToolResult::fail(ok.error().message);
    commit(s, tx, "add_constraint");
    return ToolResult::ok(
        Json(Json::Object{{"constraint", constraintJson(doc, *load(doc).find(newId))},
                          {"degreesOfFreedom", res.value().degreesOfFreedom},
                          {"changedEntities", static_cast<double>(res.value().changedEntities)}}));
}

ToolResult removeConstraint(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    ConstraintSet set = load(doc);
    const Json* idj = a.find("id");
    if (!idj || !idj->isNumber())
        return ToolResult::fail("id is required (see list_constraints)");
    const auto id = static_cast<std::uint32_t>(idj->asNumber());
    if (!set.remove(id))
        return ToolResult::fail("no constraint #" + std::to_string(id));
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "remove_constraint");
    if (auto ok = store(tx, set); !ok)
        return ToolResult::fail(ok.error().message);
    commit(s, tx, "remove_constraint");
    return ToolResult::ok(Json(Json::Object{{"removed", static_cast<double>(id)}}));
}

ToolResult solveConstraints(ToolContext& ctx, const Json&) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    ConstraintSet set = load(doc);
    if (set.empty())
        return ToolResult::fail("the drawing has no constraints");
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "solve_constraints");
    auto res = solve(tx, set);
    if (!res)
        return ToolResult::fail(res.error().message);
    if (!res.value().converged)
        return ToolResult::fail(failText(res.value(), doc, set));
    commit(s, tx, "solve_constraints");
    return ToolResult::ok(
        Json(Json::Object{{"degreesOfFreedom", res.value().degreesOfFreedom},
                          {"changedEntities", static_cast<double>(res.value().changedEntities)}}));
}

}  // namespace

void registerConstraintTools(ToolSet& tools) {
    Json ref = tAny("entity id, or {id, part: whole|start|end|center}");
    tools.add({"list_constraints", "List constraints",
               "Lists the geometric and dimensional constraints of the drawing.", tObject({}),
               outSchema({"constraints"}), Edit::None, false, true, false, listConstraints});
    tools.add({"analyze_constraints", "Analyze constraints",
               "Reports whether the constraints are satisfied, how many degrees of freedom are "
               "left (overall and per object; 0 = fully constrained), and which constraints are "
               "redundant or in conflict.",
               tObject({}), outSchema({"summary", "solved", "degreesOfFreedom"}), Edit::None, false,
               true, false, analyzeConstraints});
    tools.add({"add_constraint", "Add constraint",
               "Adds a constraint and solves, moving the geometry as little as possible. Kinds: "
               "Coincident (a, b points), Horizontal/Vertical (a line, or a and b points), "
               "Parallel, Perpendicular, Equal, Tangent (a, b), Fix (a), Distance (a, b points; "
               "value mm), Radius (a; value mm), Angle (a, b lines; value degrees), Concentric, "
               "PointOnObject (a point, b line/circle), Midpoint "
               "(a point, b line), Collinear, DistanceRange / AngleRange (limits value..value2: "
               "joint limits). A reference may carry index: the vertex (part "
               "start) or segment (part whole) of a polyline, from 0; inserts are points (start) "
               "and directions (whole); their index 0 is the insertion point and k the k-th POINT "
               "inside the block. Fails, "
               "leaving the drawing unchanged, when the constraints conflict.",
               tObject({{"kind", tEnum({"Coincident", "Horizontal", "Vertical", "Parallel",
                                        "Perpendicular", "Equal", "Tangent", "Fix", "Distance",
                                        "Radius", "Angle", "Concentric", "PointOnObject",
                                        "Midpoint", "Collinear", "DistanceRange", "AngleRange"})},
                        {"a", ref},
                        {"b", ref},
                        {"value", tNumber("mm, or degrees for Angle; the lower limit of a range")},
                        {"value2", tNumber("the upper limit of DistanceRange / AngleRange")}},
                       {"kind", "a"}),
               outSchema({"constraint", "degreesOfFreedom"}), Edit::Document, false, false, true,
               addConstraint});
    tools.add({"remove_constraint", "Remove constraint", "Deletes a constraint by id.",
               tObject({{"id", tNumber("constraint id from list_constraints")}}, {"id"}),
               outSchema({"removed"}), Edit::Document, true, false, true, removeConstraint});
    tools.add({"solve_constraints", "Solve constraints",
               "Solves the constraints again, e.g. after entities were edited.", tObject({}),
               outSchema({"degreesOfFreedom"}), Edit::Document, false, true, true,
               solveConstraints});
}

}  // namespace tbcad::mcp::detail
