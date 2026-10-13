#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

const std::string kLabelPrefix = kAiLabelPrefix;

void commit(DrawingSession& s, Transaction& tx, const std::string& tool) {
    ChangeSet cs = tx.commit();
    s.undoStack().push(kLabelPrefix + tool, {std::move(cs)});
}

const char* viewName(LevelView v) {
    switch (v) {
        case LevelView::DimOthers:
            return "dim";
        case LevelView::HideOthers:
            return "hide";
        case LevelView::ShowAll:
            return "all";
    }
    return "dim";
}

std::size_t entitiesOnLevel(const Document& doc, LevelId id) {
    std::size_t n = 0;
    doc.forEachEntity([&](const Entity& e) {
        if (e.common.level == id && !doc.ownerBlock(e.common.id))
            ++n;
    });
    return n;
}

Json levelInfo(const Document& doc, const Level& l) {
    return Json(Json::Object{{"name", l.name},
                             {"elevation", l.elevation},
                             {"visible", l.visible},
                             {"locked", l.locked},
                             {"current", l.id == doc.header().currentLevel},
                             {"entityCount", static_cast<double>(entitiesOnLevel(doc, l.id))}});
}

ToolResult listLevels(ToolContext& ctx, const Json&) {
    const Document& doc = ctx.session.document();
    std::vector<const Level*> rows;
    for (const auto& [id, l] : doc.levels())
        rows.push_back(&l);
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Level* a, const Level* b) { return a->elevation < b->elevation; });
    Json::Array out;
    for (const Level* l : rows)
        out.push_back(levelInfo(doc, *l));
    const Level* cur = doc.level(doc.header().currentLevel);
    return ToolResult::ok(Json(Json::Object{{"levels", Json(std::move(out))},
                                            {"currentLevel", cur ? cur->name : std::string()},
                                            {"view", viewName(doc.header().levelView)}}));
}

const Level* findLevel(const Document& doc, const Json& a, const char* key = "name") {
    return doc.levelByName(argString(a, key));
}

ToolResult createLevel(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    Level row;
    row.name = argString(a, "name");
    row.elevation = argNumber(a, "elevation", 0);
    if (doc.levelByName(row.name))
        return ToolResult::fail("level \"" + row.name + "\" already exists");
    auto tx = doc.begin(kLabelPrefix + "create_level");
    auto id = tx.addLevel(row);
    if (!id)
        return ToolResult::fail(id.error().message);
    if (argBool(a, "make_current")) {
        LevelId cur = id.value();
        auto m = tx.modifyHeader([&](Header& h) { h.currentLevel = cur; });
        if (!m)
            return ToolResult::fail(m.error().message);
    }
    commit(s, tx, "create_level");
    return ToolResult::ok(levelInfo(doc, *doc.level(id.value())));
}

ToolResult modifyLevel(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const Level* l = findLevel(doc, a);
    if (!l)
        return ToolResult::fail("unknown level \"" + argString(a, "name") + "\"");
    LevelId id = l->id;
    if (a.find("visible") && !argBool(a, "visible", true) && id == doc.header().currentLevel)
        return ToolResult::fail("the current level cannot be hidden");
    auto tx = doc.begin(kLabelPrefix + "modify_level");
    auto m = tx.modifyLevel(id, [&](Level& row) {
        if (const Json* v = a.find("rename"); v && v->isString())
            row.name = v->asString();
        if (a.find("elevation"))
            row.elevation = argNumber(a, "elevation", row.elevation);
        if (a.find("visible"))
            row.visible = argBool(a, "visible", row.visible);
        if (a.find("locked"))
            row.locked = argBool(a, "locked", row.locked);
    });
    if (!m)
        return ToolResult::fail(m.error().message);
    commit(s, tx, "modify_level");
    return ToolResult::ok(levelInfo(doc, *doc.level(id)));
}

ToolResult deleteLevel(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const Level* l = findLevel(doc, a);
    if (!l)
        return ToolResult::fail("unknown level \"" + argString(a, "name") + "\"");
    if (l->id == kLevelDefault)
        return ToolResult::fail("the first level cannot be deleted");
    LevelId id = l->id;
    LevelId target = kLevelDefault;
    if (a.find("move_to")) {
        const Level* t = findLevel(doc, a, "move_to");
        if (!t)
            return ToolResult::fail("unknown level \"" + argString(a, "move_to") + "\"");
        target = t->id;
    }
    if (target == id)
        return ToolResult::fail("move_to must be a different level");
    std::vector<EntityId> ids;
    doc.forEachEntity([&](const Entity& e) {
        if (e.common.level == id)
            ids.push_back(e.common.id);
    });
    auto tx = doc.begin(kLabelPrefix + "delete_level");
    for (EntityId e : ids) {
        auto m = tx.modify(e, [&](Entity& x) { x.common.level = target; });
        if (!m)
            return ToolResult::fail(m.error().message);
    }
    if (doc.header().currentLevel == id) {
        auto m = tx.modifyHeader([&](Header& h) { h.currentLevel = target; });
        if (!m)
            return ToolResult::fail(m.error().message);
    }
    auto e = tx.eraseLevel(id);
    if (!e)
        return ToolResult::fail(e.error().message);
    commit(s, tx, "delete_level");
    return ToolResult::ok(
        Json(Json::Object{{"deleted", 1.0}, {"entitiesMoved", static_cast<double>(ids.size())}}));
}

ToolResult setCurrentLevel(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    const Level* l = findLevel(doc, a);
    if (!l)
        return ToolResult::fail("unknown level \"" + argString(a, "name") + "\"");
    LevelId id = l->id;
    auto tx = doc.begin(kLabelPrefix + "set_current_level");
    if (!l->visible) {
        auto v = tx.modifyLevel(id, [](Level& row) { row.visible = true; });
        if (!v)
            return ToolResult::fail(v.error().message);
    }
    auto m = tx.modifyHeader([&](Header& h) { h.currentLevel = id; });
    if (!m)
        return ToolResult::fail(m.error().message);
    commit(s, tx, "set_current_level");
    return ToolResult::ok(Json(Json::Object{{"currentLevel", doc.level(id)->name}}));
}

ToolResult setLevelView(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string mode = argString(a, "mode");
    LevelView v;
    if (mode == "dim")
        v = LevelView::DimOthers;
    else if (mode == "hide")
        v = LevelView::HideOthers;
    else if (mode == "all")
        v = LevelView::ShowAll;
    else
        return ToolResult::fail("mode must be dim, hide or all");
    auto tx = doc.begin(kLabelPrefix + "set_level_view");
    auto m = tx.modifyHeader([&](Header& h) { h.levelView = v; });
    if (!m)
        return ToolResult::fail(m.error().message);
    commit(s, tx, "set_level_view");
    return ToolResult::ok(Json(Json::Object{{"view", viewName(v)}}));
}

}  // namespace

void registerLevelTools(ToolSet& tools) {
    tools.add({"list_levels", "List levels",
               "Levels (floors) lowest first. Objects on different levels may share the same XY "
               "position. `view` says how levels other than the current one behave: dim (faded, "
               "not selectable), hide, or all (shown and editable).",
               tObject({}), outSchema({"levels", "currentLevel", "view"}), Edit::None, false, true,
               true, listLevels});

    tools.add({"create_level", "Create level",
               "Adds a level. New entities go on the current level unless they name a `level`.",
               tObject({{"name", tString()},
                        {"elevation", tNumber("mm, default 0; for ordering and display")},
                        {"make_current", tBool()}},
                       {"name"}),
               outSchema({"name"}), Edit::Document, false, false, true, createLevel});

    tools.add({"modify_level", "Modify level",
               "Renames a level or changes its elevation, visibility or lock. Objects on a hidden "
               "level are not shown; on a locked level they cannot be edited.",
               tObject({{"name", tString()},
                        {"rename", tString()},
                        {"elevation", tNumber()},
                        {"visible", tBool()},
                        {"locked", tBool()}},
                       {"name"}),
               outSchema({"name"}), Edit::Document, false, false, true, modifyLevel});

    tools.add({"delete_level", "Delete level",
               "Deletes a level; its entities move to `move_to` (default: the first level). The "
               "first level cannot be deleted.",
               tObject({{"name", tString()}, {"move_to", tString()}}, {"name"}),
               outSchema({"deleted"}), Edit::Document, true, false, true, deleteLevel});

    tools.add({"set_current_level", "Set current level",
               "New entities go on this level. Only entities on the current level can be edited "
               "(unless the view is `all`).",
               tObject({{"name", tString()}}, {"name"}), outSchema({"currentLevel"}),
               Edit::Document, false, true, true, setCurrentLevel});

    tools.add({"set_level_view", "Set how other levels show",
               "dim (default): other levels are faded and read-only; hide: not shown; all: shown "
               "and editable.",
               tObject({{"mode", tEnum({"dim", "hide", "all"})}}, {"mode"}), outSchema({"view"}),
               Edit::Document, false, true, true, setLevelView});
}

}  // namespace tbcad::mcp::detail
