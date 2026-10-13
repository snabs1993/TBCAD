// Tools that wrap block and layer-state commands: create_block and layer_state.

#include <algorithm>

#include "tools_common.h"

namespace tbcad::mcp::detail {

std::string pointInput(Vec2 p) {
    return formatDouble(p.x) + "," + formatDouble(p.y);
}

// Runs a command and turns a failure into a tool error naming the prompts it saw.
ToolResult runChecked(DrawingSession& s, const std::string& command, std::vector<Json> inputs,
                      Json::Object extra) {
    ChangeRecorder rec(s.document());
    CommandRunReport rep = runScripted(s, command, inputs, false);
    if (!rep.error.empty()) {
        std::string text = rep.error;
        for (const std::string& m : rep.messages)
            text += " " + m;
        return ToolResult::fail(text);
    }
    Json::Array msgs;
    for (const std::string& m : rep.messages)
        msgs.emplace_back(m);
    extra["messages"] = Json(std::move(msgs));
    extra["created"] = idsJson(rec.created());
    extra["erased"] = idsJson(rec.erased());
    return ToolResult::ok(Json(std::move(extra)));
}

namespace {

ToolResult createBlock(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string name = argString(a, "name");
    if (name.empty())
        return ToolResult::fail("name is required");
    auto ids = parseIds(doc, a.find("ids"));
    if (!ids)
        return ToolResult::fail(ids.error().message);
    if (ids.value().empty())
        return ToolResult::fail("ids must list the entities that go into the block");
    for (EntityId id : ids.value())
        if (std::string why = whyReadOnly(doc, *doc.find(id)); !why.empty())
            return ToolResult::fail("entity " + std::to_string(id.value) + ": " + why);
    Vec2 base{};
    if (const Json* b = a.find("base")) {
        auto p = pointFromJson(*b);
        if (!p)
            return ToolResult::fail("base: " + p.error().message);
        base = p.value();
    }
    std::string originals = argString(a, "originals", "convert");
    std::string keyword = originals == "retain"    ? "Retain"
                          : originals == "delete"  ? "Delete"
                          : originals == "convert" ? "Convert"
                                                   : "";
    if (keyword.empty())
        return ToolResult::fail("originals must be retain, convert (default) or delete");

    std::vector<Json> inputs;
    inputs.emplace_back(name);
    const bool exists = doc.blockByName(name) != nullptr;
    if (exists) {
        if (!argBool(a, "redefine"))
            return ToolResult::fail("block \"" + name +
                                    "\" already exists; pass redefine: true to replace it");
        inputs.emplace_back("Yes");
    }
    inputs.emplace_back(pointInput(base));
    Json::Array sel;
    for (EntityId id : ids.value())
        sel.push_back(idJson(id));
    inputs.push_back(Json(Json::Object{{"select", Json(std::move(sel))}}));
    inputs.emplace_back("");  // finish the selection
    inputs.emplace_back(keyword);
    ToolResult r = runChecked(s, "BLOCK", inputs, {{"name", name}});
    if (!r.isError && !doc.blockByName(name))
        return ToolResult::fail("the BLOCK command did not create \"" + name + "\"");
    return r;
}

ToolResult layerState(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string action = argString(a, "action", "list");
    std::string name = argString(a, "name");
    if (action == "list") {
        Json::Array names;
        static const std::string prefix = "tbcad.layerstate.";
        for (const auto& [key, value] : doc.header().custom)
            if (key.rfind(prefix, 0) == 0)
                names.emplace_back(key.substr(prefix.size()));
        return ToolResult::ok(Json(Json::Object{{"states", Json(std::move(names))}}));
    }
    if (name.empty())
        return ToolResult::fail("name is required for " + action);
    std::vector<Json> inputs;
    if (action == "save") {
        inputs = {Json("Save"), Json(name)};
    } else if (action == "restore") {
        std::string parts = argString(a, "parts", "all");
        std::string kw = parts == "all"          ? "All"
                         : parts == "visibility" ? "Visibility"
                         : parts == "lock"       ? "Lock"
                         : parts == "plot"       ? "Plot"
                         : parts == "look"       ? "Look"
                                                 : "";
        if (kw.empty())
            return ToolResult::fail("parts must be all, visibility, lock, plot or look");
        inputs = {Json("Restore"), Json(name), Json(kw)};
    } else if (action == "delete") {
        inputs = {Json("Delete"), Json(name)};
    } else if (action == "rename") {
        std::string to = argString(a, "new_name");
        if (to.empty())
            return ToolResult::fail("new_name is required for rename");
        inputs = {Json("Rename"), Json(name), Json(to)};
    } else {
        return ToolResult::fail("action must be list, save, restore, delete or rename");
    }
    inputs.emplace_back("");  // leave the option loop
    ToolResult r = runChecked(s, "-LAYERSTATE", inputs);
    if (r.isError)
        return r;
    // The command reports problems (unknown state, bad name) as messages and still finishes.
    for (const Json& m : r.data.find("messages")->asArray()) {
        const std::string& t = m.asString();
        if (t.find("no layer state") != std::string::npos ||
            t.find("No layer state") != std::string::npos ||
            t.find("not found") != std::string::npos || t.find("cannot") != std::string::npos)
            return ToolResult::fail(t);
    }
    return r;
}

Result<std::pair<Vec2, Vec2>> windowFromJson(const Json* j, const char* what) {
    if (!j || !j->isArray() || j->asArray().size() != 2)
        return makeError(ErrorCode::InvalidArgument,
                         std::string(what) + " must be two corners [[x1,y1],[x2,y2]]");
    auto p1 = pointFromJson(j->asArray()[0]);
    auto p2 = pointFromJson(j->asArray()[1]);
    if (!p1 || !p2)
        return makeError(ErrorCode::InvalidArgument, std::string(what) + ": bad corner");
    return std::make_pair(p1.value(), p2.value());
}

std::size_t dynCount(const Document& doc, const std::string& block, bool actions) {
    const BlockDef* d = doc.blockByName(block);
    if (!d || !d->dynamic)
        return 0;
    return actions ? d->dynamic->actions.size() : d->dynamic->params.size();
}

ToolResult noteIfUnchanged(ToolResult r, bool changed) {
    if (r.isError || changed)
        return r;
    std::string why = "nothing was added";
    if (const Json* m = r.data.find("messages"))
        if (m->isArray() && !m->asArray().empty())
            why = m->asArray().back().asString();
    return ToolResult::fail(why);
}

ToolResult addBlockParameter(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string block = argString(a, "block");
    std::string type = argString(a, "type");
    std::string name = argString(a, "name");
    if (!doc.blockByName(block))
        return ToolResult::fail("no block named \"" + block + "\"");
    std::vector<Json> in{Json(block)};
    auto pt = [&](const char* key) -> Result<std::string> {
        const Json* j = a.find(key);
        if (!j)
            return makeError(ErrorCode::InvalidArgument, std::string(key) + " is required");
        auto p = pointFromJson(*j);
        if (!p)
            return makeError(ErrorCode::InvalidArgument, std::string(key) + ": bad point");
        return pointInput(p.value());
    };
    if (type == "linear" || type == "flip" || type == "rotation") {
        in.emplace_back(type == "linear" ? "Linear" : type == "flip" ? "Flip" : "Rotation");
        in.emplace_back(name);
        const char* k1 = type == "linear" ? "start" : type == "flip" ? "line_start" : "center";
        const char* k2 = type == "linear" ? "end" : type == "flip" ? "line_end" : "handle";
        auto p1 = pt(k1);
        auto p2 = pt(k2);
        if (!p1)
            return ToolResult::fail(p1.error().message);
        if (!p2)
            return ToolResult::fail(p2.error().message);
        in.emplace_back(p1.value());
        in.emplace_back(p2.value());
        if (type == "linear") {
            in.emplace_back(formatDouble(argNumber(a, "min", 0)));
            in.emplace_back(formatDouble(argNumber(a, "max", 0)));
        }
    } else if (type == "visibility") {
        in.emplace_back("Visibility");
        in.emplace_back(name);
        const Json* states = a.find("states");
        if (!states || !states->isArray() || states->asArray().size() < 2)
            return ToolResult::fail("states needs at least two {name, window} entries");
        for (const Json& st : states->asArray()) {
            auto w = windowFromJson(st.find("window"), "state window");
            std::string sn = argString(st, "name");
            if (!w || sn.empty())
                return ToolResult::fail(
                    "each state needs a name and a window of the objects it shows");
            in.emplace_back(sn);
            in.emplace_back(pointInput(w.value().first));
            in.emplace_back(pointInput(w.value().second));
        }
        in.emplace_back("");
    } else {
        return ToolResult::fail("type must be linear, rotation, flip or visibility");
    }
    const std::size_t before = dynCount(doc, block, false);
    ToolResult r = runChecked(s, "BPARAM", in, {{"block", block}});
    return noteIfUnchanged(std::move(r), dynCount(doc, block, false) > before);
}

ToolResult addBlockAction(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string block = argString(a, "block");
    std::string param = argString(a, "parameter");
    std::string action = argString(a, "action", "");
    const BlockDef* def = doc.blockByName(block);
    if (!def)
        return ToolResult::fail("no block named \"" + block + "\"");
    std::vector<Json> in{Json(block), Json(param)};
    const DynParam* dp = nullptr;
    if (def->dynamic)
        for (const DynParam& p : def->dynamic->params)
            if (p.name == param)
                dp = &p;
    if (!dp)
        return ToolResult::fail("the block has no parameter \"" + param + "\"");
    if (dp->kind == DynParam::Kind::Linear) {
        if (action != "stretch" && action != "move" && action != "scale")
            return ToolResult::fail("action must be stretch, move or scale for a linear parameter");
        in.emplace_back(action == "stretch" ? "Stretch" : action == "move" ? "Move" : "Scale");
        if (action == "stretch") {
            auto f = windowFromJson(a.find("frame"), "frame");
            if (!f)
                return ToolResult::fail(f.error().message);
            in.emplace_back(pointInput(f.value().first));
            in.emplace_back(pointInput(f.value().second));
        }
    }
    auto w = windowFromJson(a.find("objects"), "objects");
    if (!w)
        return ToolResult::fail(w.error().message);
    in.emplace_back(pointInput(w.value().first));
    in.emplace_back(pointInput(w.value().second));
    const std::size_t before = dynCount(doc, block, true);
    ToolResult r = runChecked(s, "BACTION", in, {{"block", block}});
    return noteIfUnchanged(std::move(r), dynCount(doc, block, true) > before);
}

ToolResult setDynamicValue(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    auto id = a.find("id") ? parseId(*a.find("id")) : std::nullopt;
    const Entity* e = id ? doc.find(*id) : nullptr;
    const auto* ins = e ? entityAs<InsertData>(*e) : nullptr;
    if (!ins)
        return ToolResult::fail("id must be an insert of a dynamic block");
    std::string param = argString(a, "parameter");
    const Json* v = a.find("value");
    if (!v)
        return ToolResult::fail("value is required");
    std::string text;
    if (v->isString())
        text = v->asString();
    else if (v->isBool())
        text = v->asBool() ? "Yes" : "No";
    else if (v->isNumber())
        text = formatDouble(v->asNumber());
    else
        return ToolResult::fail("value must be a number, boolean or state name");
    std::vector<Json> in;
    in.push_back(Json(Json::Object{{"pick", idJson(*id)}}));
    in.emplace_back(param);
    in.emplace_back(text);
    ToolResult r = runChecked(s, "DYNSET", in, {{"id", idJson(*id)}});
    if (r.isError)
        return r;
    for (const Json& m : r.data.find("messages")->asArray())
        if (m.asString().find("No such") != std::string::npos ||
            m.asString().find("cannot") != std::string::npos)
            return ToolResult::fail(m.asString());
    return r;
}

}  // namespace

void registerBlockTools(ToolSet& tools) {
    Tool block{"create_block",
               "Create block",
               "Makes a block from existing entities (the BLOCK command). `originals`: convert "
               "(default; the entities become an insert of the block), retain or delete. Existing "
               "names need redefine: true. Entities that are attribute definitions (text with "
               "`attdef`) become the block's attributes. Insert it with add_entities kind insert.",
               tObject({{"name", tString()},
                        {"ids", tArray(tString(), "entity ids that go into the block")},
                        {"base", tPoint("base point; default [0, 0]")},
                        {"originals", tEnum({"convert", "retain", "delete"})},
                        {"redefine", tBool()}},
                       {"name", "ids"}),
               outSchema({"name", "created", "erased", "messages"}),
               Edit::Document,
               false,
               false,
               true,
               createBlock};
    block.available = [](DrawingSession& s) { return s.registry().find("BLOCK") != nullptr; };
    tools.add(std::move(block));

    Tool state{"layer_state",
               "Layer states",
               "Saves, restores, deletes, renames or lists named layer states (which layers are "
               "on/frozen/locked/plot and their colour, linetype, lineweight). `restore` can apply "
               "only some `parts`. list works everywhere; the rest need the -LAYERSTATE command.",
               tObject({{"action", tEnum({"list", "save", "restore", "delete", "rename"})},
                        {"name", tString()},
                        {"new_name", tString("for rename")},
                        {"parts", tEnum({"all", "visibility", "lock", "plot", "look"},
                                        "for restore; default all")}}),
               outSchema({"states", "messages"}),
               Edit::Document,
               false,
               false,
               true,
               layerState};
    tools.add(std::move(state));

    Tool bparam{"add_block_parameter",
                "Add dynamic block parameter",
                "Adds a dynamic parameter to a block (BPARAM). linear: start, end (block "
                "coordinates), optional min/max; rotation: center, handle; flip: line_start, "
                "line_end; visibility: states [{name, window: [[x,y],[x,y]]}] where the window "
                "encloses the objects that state shows.",
                tObject({{"block", tString()},
                         {"type", tEnum({"linear", "rotation", "flip", "visibility"})},
                         {"name", tString("parameter name; default is the type")},
                         {"start", tPoint()},
                         {"end", tPoint()},
                         {"min", tNumber()},
                         {"max", tNumber()},
                         {"center", tPoint()},
                         {"handle", tPoint()},
                         {"line_start", tPoint()},
                         {"line_end", tPoint()},
                         {"states", tArray(tAny(), "visibility states")}},
                        {"block", "type"}),
                outSchema({"block", "messages"}),
                Edit::Document,
                false,
                false,
                true,
                addBlockParameter};
    bparam.available = [](DrawingSession& s) { return s.registry().find("BPARAM") != nullptr; };
    tools.add(std::move(bparam));

    Tool baction{"add_block_action",
                 "Add dynamic block action",
                 "Adds an action driven by a parameter (BACTION): stretch, move or scale for a "
                 "linear parameter (stretch also needs a `frame`), rotate and flip are implied by "
                 "their parameters. `objects` is a window of block coordinates; objects completely "
                 "inside it are changed.",
                 tObject({{"block", tString()},
                          {"parameter", tString()},
                          {"action", tEnum({"stretch", "move", "scale"})},
                          {"frame", tArray(tPoint(), "stretch frame, two corners")},
                          {"objects", tArray(tPoint(), "two corners")}},
                         {"block", "parameter", "objects"}),
                 outSchema({"block", "messages"}),
                 Edit::Document,
                 false,
                 false,
                 true,
                 addBlockAction};
    baction.available = [](DrawingSession& s) { return s.registry().find("BACTION") != nullptr; };
    tools.add(std::move(baction));

    Tool dynset{"set_dynamic_value",
                "Set dynamic block value",
                "Sets one parameter of an inserted dynamic block (DYNSET): a distance (mm) for "
                "linear, an angle for rotation, true/false for flip, or a state name for "
                "visibility.",
                tObject({{"id", tString("insert entity id")},
                         {"parameter", tString()},
                         {"value", tAny("number, boolean or state name")}},
                        {"id", "parameter", "value"}),
                outSchema({"id", "messages"}),
                Edit::Document,
                false,
                true,
                true,
                setDynamicValue};
    dynset.available = [](DrawingSession& s) { return s.registry().find("DYNSET") != nullptr; };
    tools.add(std::move(dynset));
}

}  // namespace tbcad::mcp::detail
