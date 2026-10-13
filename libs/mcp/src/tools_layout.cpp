// Paper-space layouts, viewports and title blocks (libs/layout) as tools.

#include <cmath>

#include "tbcad/layout/layout.h"
#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

void commit(DrawingSession& s, Transaction& tx, const std::string& tool) {
    ChangeSet cs = tx.commit();
    s.undoStack().push(std::string(kAiLabelPrefix) + tool, {std::move(cs)});
}

Json layoutJson(const Document& doc, const layout::Layout& l) {
    Json::Array vps;
    for (const auto& v : l.viewports) {
        Json::Array frozen;
        for (LayerId id : v.frozen)
            frozen.emplace_back(layerPath(doc, id));
        vps.push_back(Json(Json::Object{
            {"id", v.id},
            {"rect", Json(Json::Array{pointToJson(v.paper.min), pointToJson(v.paper.max)})},
            {"center", pointToJson(v.center)},
            {"scale", "1:" + formatDouble(v.scaleN)},
            {"scaleN", v.scaleN},
            {"on", v.on},
            {"frozenLayers", Json(std::move(frozen))}}));
    }
    return Json(Json::Object{{"name", l.name},
                             {"paper", l.paperName},
                             {"landscape", l.landscape},
                             {"size", pointToJson({l.widthMm, l.heightMm})},
                             {"viewports", Json(std::move(vps))}});
}

Result<layout::Layout> needLayout(const Document& doc, const std::string& name) {
    auto l = layout::findLayout(doc, name);
    if (!l) {
        std::string all;
        for (const auto& x : layout::layouts(doc))
            all += (all.empty() ? "" : ", ") + x.name;
        return makeError(ErrorCode::NotFound,
                         "no layout \"" + name + "\"" +
                             (all.empty() ? " (the drawing has none)" : " (layouts: " + all + ")"));
    }
    return *l;
}

// "1:50", "50" or a number -> N.
Result<double> scaleFromJson(const Json& j) {
    double n = 0;
    if (j.isNumber()) {
        n = j.asNumber();
    } else if (j.isString()) {
        std::string t = j.asString();
        if (t.rfind("1:", 0) == 0)
            t = t.substr(2);
        char* end = nullptr;
        n = std::strtod(t.c_str(), &end);
        if (end == t.c_str() || *end != 0)
            return makeError(ErrorCode::InvalidArgument, "scale must be like \"1:50\" or 50");
    }
    if (!(n > 0))
        return makeError(ErrorCode::InvalidArgument, "scale must be positive (1:N means N)");
    return n;
}

ToolResult listLayouts(ToolContext& ctx, const Json&) {
    Document& doc = ctx.session.document();
    Json::Array all;
    for (const auto& l : layout::layouts(doc))
        all.push_back(layoutJson(doc, l));
    return ToolResult::ok(Json(Json::Object{{"layouts", Json(std::move(all))}}));
}

ToolResult createLayout(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string name = argString(a, "name");
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "create_layout");
    auto r =
        layout::createLayout(tx, name, argString(a, "paper", "A3"), argBool(a, "landscape", true));
    if (!r)
        return ToolResult::fail(r.error().message);
    commit(s, tx, "create_layout");
    auto l = needLayout(doc, name);
    return ToolResult::ok(layoutJson(doc, l.value()));
}

ToolResult deleteLayout(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string name = argString(a, "name");
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "delete_layout");
    auto r = layout::deleteLayout(tx, name);
    if (!r)
        return ToolResult::fail(r.error().message);
    commit(s, tx, "delete_layout");
    return ToolResult::ok(Json(Json::Object{{"deleted", name}}));
}

ToolResult modifyLayout(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string name = argString(a, "name");
    auto cur = needLayout(doc, name);
    if (!cur)
        return ToolResult::fail(cur.error().message);
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "modify_layout");
    if (a.find("paper") || a.find("landscape")) {
        double w = argNumber(a, "width", 0), h = argNumber(a, "height", 0);
        auto r = layout::setLayoutPaper(tx, name, argString(a, "paper", cur.value().paperName),
                                        argBool(a, "landscape", cur.value().landscape), w, h);
        if (!r)
            return ToolResult::fail(r.error().message);
    }
    std::string finalName = name;
    if (const Json* n = a.find("new_name")) {
        auto r = layout::renameLayout(tx, name, n->asString());
        if (!r)
            return ToolResult::fail(r.error().message);
        finalName = n->asString();
    }
    commit(s, tx, "modify_layout");
    auto l = needLayout(doc, finalName);
    return ToolResult::ok(layoutJson(doc, l.value()));
}

ToolResult addViewport(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string name = argString(a, "layout");
    auto cur = needLayout(doc, name);
    if (!cur)
        return ToolResult::fail(cur.error().message);
    const Json* rect = a.find("rect");
    if (!rect || !rect->isArray() || rect->asArray().size() != 2)
        return ToolResult::fail("rect must be [[x0, y0], [x1, y1]] in mm on the sheet");
    auto p0 = pointFromJson(rect->asArray()[0]);
    auto p1 = pointFromJson(rect->asArray()[1]);
    if (!p0 || !p1)
        return ToolResult::fail("rect must be [[x0, y0], [x1, y1]] in mm on the sheet");
    BBox2 box;
    box.expand(p0.value());
    box.expand(p1.value());
    Vec2 center{};
    if (const Json* c = a.find("center")) {
        auto p = pointFromJson(*c);
        if (!p)
            return ToolResult::fail("center: " + p.error().message);
        center = p.value();
    }
    auto sc = scaleFromJson(a.find("scale") ? *a.find("scale") : Json(1.0));
    if (!sc)
        return ToolResult::fail(sc.error().message);
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "add_viewport");
    auto id = layout::addViewport(tx, name, box, center, sc.value());
    if (!id)
        return ToolResult::fail(id.error().message);
    commit(s, tx, "add_viewport");
    auto l = needLayout(doc, name);
    return ToolResult::ok(
        Json(Json::Object{{"viewport", id.value()}, {"layout", layoutJson(doc, l.value())}}));
}

ToolResult modifyViewport(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string name = argString(a, "layout");
    auto cur = needLayout(doc, name);
    if (!cur)
        return ToolResult::fail(cur.error().message);
    int id = static_cast<int>(argNumber(a, "id", 0));
    if (!cur.value().findViewport(id))
        return ToolResult::fail("layout \"" + name + "\" has no viewport " + std::to_string(id));
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "modify_viewport");
    auto check = [&](const Result<void>& r) -> std::string {
        return r ? std::string() : r.error().message;
    };
    if (argBool(a, "remove")) {
        if (std::string e = check(layout::removeViewport(tx, name, id)); !e.empty())
            return ToolResult::fail(e);
    } else {
        if (const Json* v = a.find("scale")) {
            auto sc = scaleFromJson(*v);
            if (!sc)
                return ToolResult::fail(sc.error().message);
            if (std::string e = check(layout::setViewportScale(tx, name, id, sc.value()));
                !e.empty())
                return ToolResult::fail(e);
        }
        if (const Json* v = a.find("center")) {
            auto p = pointFromJson(*v);
            if (!p)
                return ToolResult::fail("center: " + p.error().message);
            if (std::string e = check(layout::setViewportCenter(tx, name, id, p.value()));
                !e.empty())
                return ToolResult::fail(e);
        }
        if (const Json* v = a.find("on"))
            if (std::string e = check(layout::setViewportOn(tx, name, id, v->asBool(true)));
                !e.empty())
                return ToolResult::fail(e);
        for (const char* key : {"freeze", "thaw"}) {
            const Json* list = a.find(key);
            if (!list)
                continue;
            for (const Json& p : list->asArray()) {
                const Layer* layer = findLayerByPath(doc, p.asString());
                if (!layer)
                    return ToolResult::fail("unknown layer \"" + p.asString() + "\"");
                if (std::string e = check(layout::setViewportLayerFrozen(
                        tx, name, id, layer->id, std::string(key) == "freeze"));
                    !e.empty())
                    return ToolResult::fail(e);
            }
        }
    }
    commit(s, tx, "modify_viewport");
    auto l = needLayout(doc, name);
    return ToolResult::ok(layoutJson(doc, l.value()));
}

ToolResult addTitleBlock(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    Document& doc = s.document();
    std::string name = argString(a, "layout");
    auto cur = needLayout(doc, name);
    if (!cur)
        return ToolResult::fail(cur.error().message);
    layout::TitleBlockFields f;
    f.project = argString(a, "project");
    f.drawing = argString(a, "drawing");
    f.scale = argString(a, "scale");
    f.date = argString(a, "date");
    f.sheet = argString(a, "sheet");
    ChangeRecorder rec(doc);
    auto tx = doc.begin(std::string(kAiLabelPrefix) + "add_title_block");
    auto r = layout::addTitleBlock(tx, name, f);
    if (!r)
        return ToolResult::fail(r.error().message);
    commit(s, tx, "add_title_block");
    auto l = needLayout(doc, name);
    return ToolResult::ok(layoutJson(doc, l.value()));
}

}  // namespace

void registerLayoutTools(ToolSet& tools) {
    tools.add({"list_layouts", "List layouts",
               "Paper-space layouts with their sheets and viewports (rectangle on the sheet in "
               "mm, model point at the centre, scale 1:N, frozen layers).",
               tObject({}), outSchema({"layouts"}), Edit::None, false, true, true, listLayouts});

    tools.add({"create_layout", "Create layout",
               "Adds a layout (a sheet of paper) such as \"Plan A3\". paper: A0..A4 or ANSI A..E.",
               tObject({{"name", tString()},
                        {"paper", tString("default A3")},
                        {"landscape", tBool("default true")}},
                       {"name"}),
               outSchema({"name", "viewports"}), Edit::Document, false, false, true, createLayout});

    tools.add({"modify_layout", "Modify layout",
               "Renames a layout or changes its paper size and orientation. paper \"Custom\" "
               "needs width and height in mm.",
               tObject({{"name", tString()},
                        {"new_name", tString()},
                        {"paper", tString()},
                        {"landscape", tBool()},
                        {"width", tNumber()},
                        {"height", tNumber()}},
                       {"name"}),
               outSchema({"name", "viewports"}), Edit::Document, false, false, true, modifyLayout});

    tools.add({"delete_layout", "Delete layout", "Removes a layout and everything on its sheet.",
               tObject({{"name", tString()}}, {"name"}), outSchema({"deleted"}), Edit::Document,
               true, false, true, deleteLayout});

    tools.add({"add_viewport", "Add viewport",
               "Puts a window onto model space on the sheet. rect is the viewport's rectangle on "
               "the sheet in mm [[x0,y0],[x1,y1]], center the model point shown in the middle, "
               "scale \"1:50\" (or 50) the sheet scale.",
               tObject({{"layout", tString()},
                        {"rect", tArray(tPoint(), "two corners", 2)},
                        {"center", tPoint("model point; default [0, 0]")},
                        {"scale", tAny("\"1:N\" or N; default 1:1")}},
                       {"layout", "rect"}),
               outSchema({"viewport", "layout"}), Edit::Document, false, false, true, addViewport});

    tools.add(
        {"modify_viewport", "Modify viewport",
         "Changes a viewport's scale, centre, visibility or the layers frozen in it (`freeze` "
         "and `thaw` take layer paths; the layer table is not touched), or removes it.",
         tObject({{"layout", tString()},
                  {"id", tInt()},
                  {"scale", tAny()},
                  {"center", tPoint()},
                  {"on", tBool()},
                  {"freeze", tArray(tString())},
                  {"thaw", tArray(tString())},
                  {"remove", tBool()}},
                 {"layout", "id"}),
         outSchema({"name", "viewports"}), Edit::Document, false, false, true, modifyViewport});

    tools.add({"add_title_block", "Add title block",
               "Adds a framed title block (project, drawing, scale, date, sheet) to a layout.",
               tObject({{"layout", tString()},
                        {"project", tString()},
                        {"drawing", tString()},
                        {"scale", tString()},
                        {"date", tString()},
                        {"sheet", tString()}},
                       {"layout"}),
               outSchema({"name", "viewports"}), Edit::Document, false, false, true,
               addTitleBlock});
}

}  // namespace tbcad::mcp::detail
