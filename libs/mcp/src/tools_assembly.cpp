#include "tbcad/blocks/assembly.h"
#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

const std::string kSetLabel = std::string(kAiLabelPrefix) + "set_assembly_parameters";

const char* kindText(AsmParam::Kind k) {
    switch (k) {
        case AsmParam::Kind::Number:
            return "number";
        case AsmParam::Kind::Integer:
            return "integer";
        case AsmParam::Kind::Text:
            return "text";
        case AsmParam::Kind::Choice:
            return "choice";
    }
    return "number";
}

Json valueJson(const AsmParam& p, const AsmValue& v) {
    if (p.kind == AsmParam::Kind::Text || p.kind == AsmParam::Kind::Choice)
        return Json(v.text);
    return Json(v.number);
}

struct Found {
    const Entity* entity = nullptr;
    const InsertData* insert = nullptr;
    const BlockDef* def = nullptr;
    std::string error{};
};

Found findAssembly(const Document& doc, const Json& a) {
    Found f;
    const Json* idj = a.find("id");
    auto id = idj ? parseId(*idj) : std::nullopt;
    if (!id) {
        f.error = "pass `id`: the entity id of an inserted assembly";
        return f;
    }
    f.entity = doc.find(*id);
    f.insert = f.entity ? entityAs<InsertData>(*f.entity) : nullptr;
    f.def = f.insert ? doc.block(f.insert->block) : nullptr;
    if (!f.def || !f.def->assembly || doc.ownerBlock(*id))
        f.error = "entity " + std::to_string(id->value) + " is not an inserted assembly";
    return f;
}

ToolResult listParameters(ToolContext& ctx, const Json& a) {
    const Document& doc = ctx.session.document();
    Found f = findAssembly(doc, a);
    if (!f.error.empty())
        return ToolResult::fail(f.error);
    const AssemblyDef& ad = *f.def->assembly;
    const auto values = blocks::asmEffectiveValues(ad, *f.insert);
    Json::Array params;
    for (std::size_t i = 0; i < ad.params.size(); ++i) {
        const AsmParam& p = ad.params[i];
        Json::Object o{
            {"name", p.name}, {"type", kindText(p.kind)}, {"value", valueJson(p, values[i])}};
        if (p.kind == AsmParam::Kind::Number || p.kind == AsmParam::Kind::Integer) {
            o["default"] = p.defaultNumber;
            if (p.max > p.min) {
                o["min"] = p.min;
                o["max"] = p.max;
            }
        } else if (p.kind == AsmParam::Kind::Choice) {
            Json::Array c;
            for (const std::string& s : p.choices)
                c.emplace_back(s);
            o["choices"] = Json(std::move(c));
        }
        params.emplace_back(Json(std::move(o)));
    }
    Json::Array presets;
    for (const AsmPreset& p : ad.presets)
        presets.emplace_back(p.name);
    const auto res = blocks::resolveAssembly(doc, *f.def, *f.insert);
    Json::Array problems;
    for (const std::string& s : res.errors)
        problems.emplace_back(s);
    return ToolResult::ok(Json(Json::Object{{"block", f.def->name},
                                            {"parameters", Json(std::move(params))},
                                            {"presets", Json(std::move(presets))},
                                            {"problems", Json(std::move(problems))}}));
}

ToolResult setParameters(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    Found f = findAssembly(doc, a);
    if (!f.error.empty())
        return ToolResult::fail(f.error);
    const Json* values = a.find("values");
    const std::string preset = argString(a, "preset");
    if ((!values || !values->isObject()) && preset.empty())
        return ToolResult::fail("pass `values` (name -> value) and/or `preset`");
    Entity current = *f.entity;
    if (!preset.empty()) {
        auto r = blocks::asmApplyPreset(doc, current, preset);
        if (!r)
            return ToolResult::fail(r.error().message);
        current = std::move(r).value();
    }
    if (values && values->isObject()) {
        for (const auto& [name, v] : values->asObject()) {
            expr::Value value;
            if (v.isNumber())
                value = expr::Value::ofNumber(v.asNumber());
            else if (v.isString())
                value = expr::Value::ofText(v.asString());
            else if (v.isBool())
                value = expr::Value::ofNumber(v.asBool() ? 1 : 0);
            else
                return ToolResult::fail("\"" + name + "\": the value must be a number or a text");
            auto r = blocks::asmSet(doc, current, name, value);
            if (!r)
                return ToolResult::fail("\"" + name + "\": " + r.error().message);
            current = std::move(r).value();
        }
    }
    const EntityId id = f.entity->common.id;
    auto tx = doc.begin(kSetLabel);
    auto m = tx.modify(id, [&](Entity& x) { x.data = current.data; });
    if (!m)
        return ToolResult::fail(m.error().message);
    ChangeSet cs = tx.commit();
    s.undoStack().push(kSetLabel, {std::move(cs)});
    return listParameters(ctx, Json(Json::Object{{"id", idJson(id)}}));
}

}  // namespace

ToolResult assemblyBomTool(ToolContext& ctx, const Json&) {
    const auto rows = blocks::assemblyBom(ctx.session.document());
    Json::Array out;
    std::size_t total = 0;
    for (const blocks::BomRow& r : rows) {
        Json::Object values;
        for (const auto& [n, v] : r.values)
            values[n] = Json(v);
        out.push_back(Json(Json::Object{{"assembly", r.block},
                                        {"parameters", Json(std::move(values))},
                                        {"quantity", static_cast<double>(r.count)}}));
        total += r.count;
    }
    return ToolResult::ok(
        Json(Json::Object{{"lines", Json(std::move(out))}, {"total", static_cast<double>(total)}}));
}

void registerAssemblyTools(ToolSet& tools) {
    tools.add(
        {"assembly_bom", "Assembly bill of materials",
         "Lists the assemblies in the drawing, grouped by block and parameter values, with the "
         "quantity of each (nested assemblies and array items included).",
         tObject({}), outSchema({"lines", "total"}), Edit::None, false, true, false,
         assemblyBomTool});
    tools.add({"list_assembly_parameters", "List assembly parameters",
               "Lists the parameters of an inserted parametric assembly (name, type, current "
               "value, limits, choices), its presets and any formula problems. `id` is the "
               "entity id of the insert.",
               tObject({{"id", tString("entity id of the inserted assembly")}}, {"id"}),
               outSchema({"parameters"}), Edit::None, false, true, false, listParameters});
    tools.add({"set_assembly_parameters", "Set assembly parameters",
               "Changes parameters of an inserted assembly in one undo step: `values` maps "
               "parameter names to numbers or texts; `preset` applies a named preset first. The "
               "geometry follows the formulas of the assembly. Values outside a parameter's "
               "limits or choices are refused.",
               tObject({{"id", tString("entity id of the inserted assembly")},
                        {"values", tAny("object: parameter name -> number or text")},
                        {"preset", tString("name of a preset to apply first")}},
                       {"id"}),
               outSchema({"parameters"}), Edit::Document, false, false, true, setParameters});
}

}  // namespace tbcad::mcp::detail
