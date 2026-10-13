// Tools that wrap block and layer-state commands: create_block and layer_state.

#include <algorithm>

#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

std::string pointInput(Vec2 p) {
    return formatDouble(p.x) + "," + formatDouble(p.y);
}

// Runs a command and turns a failure into a tool error naming the prompts it saw.
ToolResult runChecked(DrawingSession& s, const std::string& command, std::vector<Json> inputs,
                      Json::Object extra = {}) {
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
}

}  // namespace tbcad::mcp::detail
