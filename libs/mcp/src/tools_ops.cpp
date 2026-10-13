// Selection-driven commands as tools: layer operations by object, groups, draw order, the
// clipboard, copy to level and region booleans. Each runs its command with scripted inputs, so the
// tool does exactly what the typed command does.

#include "tbcad/core/groups.h"
#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

// Commands report most problems as a message and then finish. Treat those messages as errors.
ToolResult failOnProblem(ToolResult r) {
    if (r.isError)
        return r;
    static const char* markers[] = {"Cannot",      "cannot",  "No layers", "no layer", "no object",
                                    "There is no", "already", "None of",   "empty",    "Nothing to",
                                    "Select ",     "not in",  "No such",   "is not"};
    if (const Json* m = r.data.find("messages"))
        for (const Json& line : m->asArray())
            for (const char* mk : markers)
                if (line.asString().find(mk) != std::string::npos)
                    return ToolResult::fail(line.asString());
    return r;
}

Json selectInput(const std::vector<EntityId>& ids) {
    Json::Array a;
    for (EntityId id : ids)
        a.push_back(idJson(id));
    return Json(Json::Object{{"select", Json(std::move(a))}});
}

Result<std::vector<EntityId>> needIds(const Document& doc, const Json& a, const char* key = "ids") {
    auto ids = parseIds(doc, a.find(key));
    if (!ids)
        return ids.error();
    if (ids.value().empty())
        return makeError(ErrorCode::InvalidArgument, std::string(key) + " must list entities");
    return ids;
}

Result<EntityId> needOne(const Document& doc, const Json& a, const char* key) {
    const Json* j = a.find(key);
    auto id = j ? parseId(*j) : std::nullopt;
    if (!id || !doc.find(*id))
        return makeError(ErrorCode::NotFound, std::string(key) + " must be an existing entity id");
    return *id;
}

// ---- layers by object -------------------------------------------------------------------------

ToolResult layerByObjects(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string action = argString(a, "action");
    struct Row {
        const char* action;
        const char* command;
        bool needsIds;
        bool needsTarget;
    };
    static const Row rows[] = {
        {"off", "LAYOFF", true, false},     {"freeze", "LAYFRZ", true, false},
        {"lock", "LAYLCK", true, false},    {"unlock", "LAYULK", true, false},
        {"isolate", "LAYISO", true, false}, {"unisolate", "LAYUNISO", false, false},
        {"all_on", "LAYON", false, false},  {"thaw_all", "LAYTHW", false, false},
        {"match", "LAYMCH", true, true},    {"merge", "LAYMRG", true, true}};
    const Row* row = nullptr;
    for (const Row& r : rows)
        if (action == r.action)
            row = &r;
    if (!row)
        return ToolResult::fail(
            "action must be off, freeze, lock, unlock, isolate, unisolate, all_on, thaw_all, match "
            "or merge (use delete_layer_with_objects to delete)");
    std::vector<Json> in;
    if (row->needsIds) {
        auto ids = needIds(doc, a);
        if (!ids)
            return ToolResult::fail(ids.error().message);
        in.push_back(selectInput(ids.value()));
        in.emplace_back("");
    }
    if (row->needsTarget) {
        auto t = needOne(doc, a, "target");
        if (!t)
            return ToolResult::fail(t.error().message);
        in.push_back(Json(Json::Object{{"pick", idJson(t.value())}}));
    }
    return failOnProblem(runChecked(s, row->command, in));
}

ToolResult deleteLayerWithObjects(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    auto e = needOne(s.document(), a, "id");
    if (!e)
        return ToolResult::fail(e.error().message);
    std::vector<Json> in{Json(Json::Object{{"pick", idJson(e.value())}}), Json("Yes")};
    return failOnProblem(runChecked(s, "LAYDEL", in));
}

// ---- groups, draw order -----------------------------------------------------------------------

ToolResult listGroups(ToolContext& ctx, const Json&) {
    Json::Array out;
    for (const Group& g : readGroups(ctx.session.document())) {
        Json::Array members;
        for (EntityId id : g.members)
            members.push_back(idJson(id));
        out.push_back(Json(Json::Object{{"name", g.name},
                                        {"members", Json(std::move(members))},
                                        {"selectable", g.selectable}}));
    }
    return ToolResult::ok(Json(Json::Object{{"groups", Json(std::move(out))}}));
}

ToolResult groupEntities(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    auto ids = needIds(s.document(), a);
    if (!ids)
        return ToolResult::fail(ids.error().message);
    std::string name = argString(a, "name");
    std::vector<Json> in{selectInput(ids.value()), Json(""), Json(name)};
    ToolResult r = failOnProblem(runChecked(s, "GROUP", in));
    return r;
}

ToolResult ungroup(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    auto e = needOne(s.document(), a, "id");
    if (!e)
        return ToolResult::fail(e.error().message);
    return failOnProblem(
        runChecked(s, "UNGROUP", {Json(Json::Object{{"pick", idJson(e.value())}})}));
}

ToolResult setDrawOrder(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    auto ids = needIds(s.document(), a);
    if (!ids)
        return ToolResult::fail(ids.error().message);
    std::string order = argString(a, "order");
    std::vector<Json> in{selectInput(ids.value()), Json("")};
    if (order == "front" || order == "back") {
        in.emplace_back(order == "front" ? "Front" : "Back");
    } else if (order == "above" || order == "below") {
        auto ref = needOne(s.document(), a, "reference");
        if (!ref)
            return ToolResult::fail(ref.error().message);
        in.emplace_back(order == "above" ? "Above" : "Below");
        in.push_back(Json(Json::Object{{"pick", idJson(ref.value())}}));
    } else {
        return ToolResult::fail("order must be front, back, above or below");
    }
    return failOnProblem(runChecked(s, "DRAWORDER", in));
}

// ---- clipboard --------------------------------------------------------------------------------

ToolResult clipboard(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string action = argString(a, "action");
    std::vector<Json> in;
    std::string command;
    if (action == "copy" || action == "cut") {
        auto ids = needIds(doc, a);
        if (!ids)
            return ToolResult::fail(ids.error().message);
        command = action == "copy" ? "COPYCLIP" : "CUTCLIP";
        in = {selectInput(ids.value()), Json("")};
    } else if (action == "paste" || action == "paste_block") {
        command = action == "paste" ? "PASTECLIP" : "PASTEBLOCK";
        Vec2 at{};
        if (const Json* p = a.find("point")) {
            auto pt = pointFromJson(*p);
            if (!pt)
                return ToolResult::fail("point: " + pt.error().message);
            at = pt.value();
        }
        in = {Json(pointInput(at))};
    } else if (action == "paste_original") {
        command = "PASTEORIG";
    } else {
        return ToolResult::fail("action must be copy, cut, paste, paste_block or paste_original");
    }
    return failOnProblem(runChecked(s, command, in));
}

ToolResult copyToLevel(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    auto ids = needIds(s.document(), a);
    if (!ids)
        return ToolResult::fail(ids.error().message);
    std::string level = argString(a, "level");
    if (level.empty())
        return ToolResult::fail("level is required (see list_levels)");
    return failOnProblem(
        runChecked(s, "COPYTOLEVEL", {selectInput(ids.value()), Json(""), Json(level)}));
}

// ---- regions ----------------------------------------------------------------------------------

ToolResult regionOp(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string op = argString(a, "op");
    if (op == "region") {
        auto ids = needIds(doc, a);
        if (!ids)
            return ToolResult::fail(ids.error().message);
        return failOnProblem(runChecked(s, "REGION", {selectInput(ids.value()), Json("")}));
    }
    if (op == "union" || op == "intersect") {
        auto ids = needIds(doc, a);
        if (!ids)
            return ToolResult::fail(ids.error().message);
        if (ids.value().size() < 2)
            return ToolResult::fail("ids needs at least two closed entities");
        return failOnProblem(runChecked(s, op == "union" ? "UNION" : "INTERSECT",
                                        {selectInput(ids.value()), Json("")}));
    }
    if (op == "subtract") {
        auto base = needIds(doc, a, "ids");
        auto cut = needIds(doc, a, "subtract");
        if (!base)
            return ToolResult::fail(base.error().message);
        if (!cut)
            return ToolResult::fail(cut.error().message);
        return failOnProblem(
            runChecked(s, "SUBTRACT",
                       {selectInput(base.value()), Json(""), selectInput(cut.value()), Json("")}));
    }
    return ToolResult::fail("op must be region, union, subtract or intersect");
}

}  // namespace

void registerOpsTools(ToolSet& tools) {
    auto cmdTool = [&](const char* name, const char* title, const char* desc, Json schema, Json out,
                       bool destructive, const char* command, ToolHandler handler) {
        Tool t{name,        title, desc, std::move(schema), std::move(out), Edit::Document,
               destructive, false, true, std::move(handler)};
        std::string c = command;
        t.available = [c](DrawingSession& s) { return s.registry().find(c) != nullptr; };
        tools.add(std::move(t));
    };
    Json ids = tArray(tString(), "entity ids");
    Json outMsg = outSchema({"messages"});

    cmdTool("layer_by_objects", "Layer operations by object",
            "Layer tools that work from objects instead of layer names (LAYOFF, LAYFRZ, LAYLCK, "
            "LAYULK, LAYISO, LAYUNISO, LAYON, LAYTHW, LAYMCH, LAYMRG). `ids` are objects on the "
            "layers to change; `match` moves them to the layer of `target`; `merge` merges their "
            "layers into the layer of `target`. unisolate, all_on and thaw_all need no ids.",
            tObject({{"action", tEnum({"off", "freeze", "lock", "unlock", "isolate", "unisolate",
                                       "all_on", "thaw_all", "match", "merge"})},
                     {"ids", ids},
                     {"target", tString("entity on the destination layer")}},
                    {"action"}),
            outMsg, false, "LAYOFF", layerByObjects);
    cmdTool("delete_layer_with_objects", "Delete a layer and its objects",
            "Deletes the layer of the given entity together with everything on it (LAYDEL). The "
            "current layer cannot be deleted.",
            tObject({{"id", tString("an entity on the layer to delete")}}, {"id"}), outMsg, true,
            "LAYDEL", deleteLayerWithObjects);

    tools.add({"list_groups", "List groups", "Named groups whose members select together.",
               tObject({}), outSchema({"groups"}), Edit::None, false, true, false, listGroups});
    cmdTool("group_entities", "Group entities",
            "Makes a named group (GROUP). Picking one member then selects the whole group.",
            tObject({{"ids", ids}, {"name", tString("default GROUP<n>")}}, {"ids"}), outMsg, false,
            "GROUP", groupEntities);
    cmdTool("ungroup", "Dissolve a group", "Dissolves the group of the given entity (UNGROUP).",
            tObject({{"id", tString("a member of the group")}}, {"id"}), outMsg, false, "UNGROUP",
            ungroup);
    cmdTool("set_draw_order", "Change draw order",
            "Moves entities to the front or back, or directly above/below a reference entity "
            "(DRAWORDER). Their relative order is kept.",
            tObject({{"ids", ids},
                     {"order", tEnum({"front", "back", "above", "below"})},
                     {"reference", tString("for above/below")}},
                    {"ids", "order"}),
            outMsg, false, "DRAWORDER", setDrawOrder);
    cmdTool("clipboard", "Clipboard",
            "copy / cut take `ids`; paste puts the clip with its base point at `point` (default "
            "0,0); paste_block pastes as one new block; paste_original keeps the original "
            "coordinates. The clip lives in the server and works across drawings.",
            tObject({{"action", tEnum({"copy", "cut", "paste", "paste_block", "paste_original"})},
                     {"ids", ids},
                     {"point", tPoint()}},
                    {"action"}),
            outSchema({"messages", "created"}), false, "COPYCLIP", clipboard);
    cmdTool("copy_to_level", "Copy to level",
            "Copies entities to another level at the same position (COPYTOLEVEL).",
            tObject({{"ids", ids}, {"level", tString("level name")}}, {"ids", "level"}),
            outSchema({"messages", "created"}), false, "COPYTOLEVEL", copyToLevel);
    cmdTool("region_op", "Region booleans",
            "op region: closed objects become plain closed polylines. union / intersect combine "
            "`ids`; subtract cuts `subtract` out of `ids`. Sources are replaced by the result "
            "(REGION, UNION, INTERSECT, SUBTRACT).",
            tObject({{"op", tEnum({"region", "union", "subtract", "intersect"})},
                     {"ids", ids},
                     {"subtract", ids}},
                    {"op", "ids"}),
            outSchema({"messages", "created", "erased"}), true, "UNION", regionOp);
}

}  // namespace tbcad::mcp::detail
