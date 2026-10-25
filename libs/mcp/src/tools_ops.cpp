// Selection-driven commands as tools: layer operations by object, groups, draw order, the
// clipboard, copy to level and region booleans. Each runs its command with scripted inputs, so the
// tool does exactly what the typed command does.

#include "tbcad/core/groups.h"
#include "tbcad/layout/layout.h"
#include "tbcad/layout/pagesetup.h"
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
        // These four ask again after each pick (Enter ends them).
        if (action == "off" || action == "freeze" || action == "lock" || action == "unlock")
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

// ---- page setups and plot style tables --------------------------------------------------------

Json pageSetupJson(const Document& doc, const layout::PageSetup& p) {
    Json::Array users;
    if (p.name == layout::modelPageSetup(doc))
        users.emplace_back("Model");
    for (const auto& l : layout::layouts(doc))
        if (l.pageSetup == p.name)
            users.emplace_back(l.name);
    return Json(Json::Object{{"name", p.name},
                             {"paper", p.settings.paperName},
                             {"landscape", p.settings.landscape},
                             {"styleTable", p.styleName},
                             {"usedBy", Json(std::move(users))}});
}

ToolResult pageSetup(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string action = argString(a, "action", "list");
    std::string name = argString(a, "name");
    if (action == "list") {
        Json::Array all;
        for (const auto& p : layout::pageSetups(doc))
            all.push_back(pageSetupJson(doc, p));
        return ToolResult::ok(Json(Json::Object{{"pageSetups", Json(std::move(all))}}));
    }
    std::vector<Json> in;
    if (action == "save") {
        in = {Json("Save"), Json(name)};
    } else if (action == "apply") {
        in = {Json("Apply"), Json(name)};
    } else if (action == "assign") {
        in = {Json("Assign"), Json(name), Json(argString(a, "layout", "Model"))};
    } else if (action == "rename") {
        std::string to = argString(a, "new_name");
        if (to.empty())
            return ToolResult::fail("new_name is required");
        in = {Json("Rename"), Json(name), Json(to)};
    } else if (action == "delete") {
        in = {Json("Delete"), Json(name)};
    } else {
        return ToolResult::fail("action must be list, save, apply, assign, rename or delete");
    }
    if (name.empty() && action != "assign")
        return ToolResult::fail("name is required for " + action);
    ToolResult r = failOnProblem(runChecked(s, "PAGESETUP", in));
    if (r.isError)
        return r;
    // The command reports a missing setup or layout as a message.
    for (const Json& m : r.data.find("messages")->asArray())
        if (m.asString().rfind("No page setup", 0) == 0 ||
            m.asString().find("no layout") != std::string::npos)
            return ToolResult::fail(m.asString());
    Json::Array all;
    for (const auto& p : layout::pageSetups(doc))
        all.push_back(pageSetupJson(doc, p));
    return ToolResult::ok(Json(Json::Object{{"pageSetups", Json(std::move(all))},
                                            {"messages", *r.data.find("messages")}}));
}

ToolResult plotStyle(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    std::string action = argString(a, "action", "list");
    std::vector<Json> in;
    if (action == "list") {
        in = {Json("List")};
    } else if (action == "select") {
        in = {Json("Select"), Json(Json::Object{{"text", argString(a, "name")}})};
    } else if (action == "import") {
        std::string path = argString(a, "path");
        if (path.empty())
            return ToolResult::fail("path of the .ctb file is required");
        in = {Json("Import"), Json(path)};
    } else {
        return ToolResult::fail("action must be list, select or import");
    }
    ToolResult r = runChecked(s, "PLOTSTYLE", in);
    if (r.isError || action == "list")
        return r;
    for (const Json& m : r.data.find("messages")->asArray()) {
        const std::string& t = m.asString();
        if (t.find("not ") != std::string::npos || t.find("annot") != std::string::npos ||
            t.find("rror") != std::string::npos || t.find("no ") != std::string::npos ||
            t.find("There is") != std::string::npos)
            return ToolResult::fail(t);
    }
    return r;
}

// ---- annotative symbols -----------------------------------------------------------------------

struct SymbolRow {
    const char* name;
    const char* command;
    std::vector<const char*> attributes;  // in the order the command asks for them
};

const std::vector<SymbolRow>& symbolRows() {
    static const std::vector<SymbolRow> rows{{"north_arrow", "NORTHARROW", {}},
                                             {"grid_bubble", "GRIDBUBBLE", {"ID"}},
                                             {"level_mark", "LEVELMARK", {"LEVEL"}},
                                             {"section_mark", "SECTIONMARK", {"SECTION", "SHEET"}},
                                             {"elevation_mark", "ELEVATIONMARK", {"VIEW", "SHEET"}},
                                             {"detail_mark", "DETAILMARK", {"DETAIL", "SHEET"}}};
    return rows;
}

ToolResult insertSymbol(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    std::string name = argString(a, "symbol");
    const SymbolRow* row = nullptr;
    for (const SymbolRow& r : symbolRows())
        if (name == r.name)
            row = &r;
    if (!row)
        return ToolResult::fail(
            "symbol must be north_arrow, grid_bubble, level_mark, section_mark, elevation_mark or "
            "detail_mark");
    const Json* pj = a.find("position");
    auto p = pj ? pointFromJson(*pj)
                : Result<Vec2>(makeError(ErrorCode::InvalidArgument, "position is required"));
    if (!p)
        return ToolResult::fail("position: " + p.error().message);
    std::vector<Json> in{Json(pointInput(p.value())),
                         Json(formatDouble(argNumber(a, "rotation", 0)))};
    const Json* attrs = a.find("attributes");
    for (const char* tag : row->attributes) {
        const Json* v = attrs ? attrs->find(tag) : nullptr;
        in.push_back(Json(Json::Object{{"text", v && v->isString() ? v->asString() : ""}}));
    }
    return failOnProblem(runChecked(s, row->command, in));
}

// ---- annotation scale -------------------------------------------------------------------------

ToolResult annotation(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string action = argString(a, "action", "get");
    auto state = [&]() {
        Json::Array list;
        for (double n : annotationScaleList(doc.header()))
            list.emplace_back(n);
        return Json::Object{{"scale", currentAnnotationScale(doc.header())},
                            {"newObjectsAnnotative", annotateNewObjects(doc.header())},
                            {"scaleList", Json(std::move(list))}};
    };
    if (action == "get")
        return ToolResult::ok(Json(state()));
    auto scaleArg = [&](const char* key) -> Result<double> {
        const Json* v = a.find(key);
        if (!v || !v->isNumber() || !(v->asNumber() > 0))
            return makeError(ErrorCode::InvalidArgument,
                             std::string(key) + " must be a number above zero (N of 1:N)");
        return v->asNumber();
    };
    ToolResult r = ToolResult::fail("nothing to do");
    if (action == "set_scale") {
        auto n = scaleArg("scale");
        if (!n)
            return ToolResult::fail(n.error().message);
        r = runChecked(s, "CANNOSCALE", {Json(formatDouble(n.value()))});
    } else if (action == "new_objects") {
        r = runChecked(s, "ANNONEW", {Json(argBool(a, "on", true) ? "On" : "Off")});
    } else if (action == "add_scale" || action == "delete_scale") {
        auto n = scaleArg("scale");
        if (!n)
            return ToolResult::fail(n.error().message);
        r = runChecked(s, "SCALELISTEDIT",
                       {Json(action == "add_scale" ? "Add" : "Delete"),
                        Json(formatDouble(n.value())), Json("Done")});
    } else if (action == "reset_scales") {
        r = runChecked(s, "SCALELISTEDIT", {Json("Reset"), Json("Done")});
    } else if (action == "set_annotative") {
        auto ids = needIds(doc, a);
        if (!ids)
            return ToolResult::fail(ids.error().message);
        const bool on = argBool(a, "on", true);
        std::vector<Json> in{selectInput(ids.value()), Json(""),
                             Json(!on                ? "Off"
                                  : a.find("scales") ? "Scales"
                                                     : "On")};
        if (on && a.find("scales")) {
            std::string joined;
            for (const Json& n : a.find("scales")->asArray())
                joined += (joined.empty() ? "" : ",") + formatDouble(n.asNumber());
            in.emplace_back(joined);
        }
        r = runChecked(s, "ANNOTATIVE", in);
    } else {
        return ToolResult::fail(
            "action must be get, set_scale, new_objects, add_scale, delete_scale, reset_scales or "
            "set_annotative");
    }
    r = failOnProblem(std::move(r));
    if (r.isError)
        return r;
    Json::Object out = state();
    out["messages"] = *r.data.find("messages");
    return ToolResult::ok(Json(std::move(out)));
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
    cmdTool("page_setup", "Page setups",
            "Named bundles of plot options saved in the drawing. list (default) shows them and "
            "which layouts use each; save stores the current plot options under `name`; apply "
            "makes one the current plot options; assign gives it to `layout` (\"Model\" for model "
            "space; an empty `name` removes it); rename; delete.",
            tObject({{"action", tEnum({"list", "save", "apply", "assign", "rename", "delete"})},
                     {"name", tString()},
                     {"new_name", tString("for rename")},
                     {"layout", tString("for assign; default Model")}},
                    {}),
            outSchema({"pageSetups"}), false, "PAGESETUP", pageSetup);
    cmdTool("plot_style", "Plot style tables",
            "list the available plot style tables (the current one is marked *), select one by "
            "name, file path or None, or import a .ctb file. The selection applies to export_pdf "
            "and export_image unless they pass `style`.",
            tObject({{"action", tEnum({"list", "select", "import"})},
                     {"name", tString("table name, file or None")},
                     {"path", tString("absolute path of a .ctb file, for import")}},
                    {}),
            outMsg, false, "PLOTSTYLE", plotStyle);
    cmdTool("insert_symbol", "Insert an annotative symbol",
            "Puts in an annotative drafting symbol at the current annotation scale: north_arrow, "
            "grid_bubble (ID), level_mark (LEVEL), section_mark (SECTION, SHEET), elevation_mark "
            "(VIEW, SHEET) or detail_mark (DETAIL, SHEET). `attributes` gives the values; "
            "`rotation` is in degrees.",
            tObject({{"symbol", tEnum({"north_arrow", "grid_bubble", "level_mark", "section_mark",
                                       "elevation_mark", "detail_mark"})},
                     {"position", tPoint()},
                     {"rotation", tNumber("degrees")},
                     {"attributes", tAny("object {TAG: value}")}},
                    {"symbol", "position"}),
            outSchema({"messages", "created"}), false, "NORTHARROW", insertSymbol);
    cmdTool("annotation", "Annotation scale",
            "get: current annotation scale, whether new text/dimensions start annotative, and the "
            "scale list. set_scale (CANNOSCALE), new_objects on/off (ANNONEW), add_scale / "
            "delete_scale / reset_scales (SCALELISTEDIT), and set_annotative to make `ids` "
            "annotative (on) or ordinary (off), optionally with `scales` (N of 1:N). Entities "
            "show `annotative` and `annotativeScales`.",
            tObject({{"action", tEnum({"get", "set_scale", "new_objects", "add_scale",
                                       "delete_scale", "reset_scales", "set_annotative"})},
                     {"scale", tNumber("N of 1:N")},
                     {"on", tBool()},
                     {"ids", ids},
                     {"scales", tArray(tNumber(), "N of 1:N for each scale")}},
                    {}),
            outSchema({"scale", "newObjectsAnnotative", "scaleList"}), false, "CANNOSCALE",
            annotation);
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
