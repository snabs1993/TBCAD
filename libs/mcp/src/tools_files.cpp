#include <filesystem>

#include "tbcad/platform/paths.h"
#include "tools_common.h"

namespace tbcad::mcp::detail {

namespace {

bool endsWithExt(const std::string& p, const std::string& ext) {
    if (p.size() < ext.size())
        return false;
    for (std::size_t i = 0; i < ext.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(p[p.size() - ext.size() + i])) != ext[i])
            return false;
    }
    return true;
}

Result<std::filesystem::path> checkPath(const std::string& text) {
    if (text.empty())
        return makeError(ErrorCode::InvalidArgument, "path is empty");
    if (!endsWithExt(text, ".tbcad") && !endsWithExt(text, ".dxf"))
        return makeError(ErrorCode::InvalidArgument, "path must end with .dxf or .tbcad");
    auto p = platform::pathFromUtf8(text);
    if (!p.is_absolute())
        return makeError(ErrorCode::InvalidArgument, "path must be absolute");
    return p;
}

bool startsWithAi(const std::string& label) {
    return label.rfind(kAiLabelPrefix, 0) == 0;
}

ToolResult stepTool(ToolContext& ctx, const Json& a, bool undo) {
    DrawingSession& s = ctx.session;
    int steps = static_cast<int>(argNumber(a, "steps", 1));
    bool force = argBool(a, "force");
    Json::Array labels;
    std::string stopped;
    for (int i = 0; i < steps; ++i) {
        std::string label = undo ? s.undoStack().undoLabel() : s.undoStack().redoLabel();
        bool available = undo ? s.undoStack().canUndo() : s.undoStack().canRedo();
        if (!available) {
            stopped = undo ? "nothing more to undo" : "nothing more to redo";
            break;
        }
        if (undo && !force && !startsWithAi(label)) {
            stopped = "the next step is \"" + label +
                      "\", which the user made; pass force: true to undo it as well";
            break;
        }
        if (!(undo ? s.undo() : s.redo())) {
            stopped = "TBCAD refused the step";
            break;
        }
        labels.emplace_back(label);
    }
    if (labels.empty())
        return ToolResult::fail(undo ? "Nothing was undone: " + stopped
                                     : "Nothing was redone: " + stopped);
    Json::Object o;
    o[undo ? "undone" : "redone"] = static_cast<double>(labels.size());
    o["labels"] = Json(std::move(labels));
    o["nextUndo"] = s.undoStack().undoLabel();
    o["nextRedo"] = s.undoStack().redoLabel();
    if (!stopped.empty())
        o["stoppedBecause"] = stopped;
    return ToolResult::ok(Json(std::move(o)));
}

ToolResult saveDrawing(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    std::string text = argString(a, "path");
    std::optional<std::filesystem::path> as;
    if (!text.empty()) {
        auto p = checkPath(text);
        if (!p)
            return ToolResult::fail(p.error().message);
        std::error_code ec;
        if (std::filesystem::exists(p.value(), ec) && !argBool(a, "overwrite") &&
            !(s.file().hasFile && s.file().path == p.value())) {
            return ToolResult::fail("the file exists; pass overwrite: true to replace it");
        }
        as = p.value();
    } else if (!s.file().hasFile) {
        return ToolResult::fail(
            "this drawing has no file yet; pass a path ending in .dxf or .tbcad");
    }
    auto r = s.save(as);
    if (!r)
        return ToolResult::fail(r.error().message);
    FileInfo f = s.file();
    return ToolResult::ok(Json(
        Json::Object{{"saved", true}, {"path", platform::pathToUtf8(f.path)}, {"name", f.name}}));
}

ToolResult openDrawing(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    auto p = checkPath(argString(a, "path"));
    if (!p)
        return ToolResult::fail(p.error().message);
    if (s.file().modified && !argBool(a, "discard_changes"))
        return ToolResult::fail(
            "the open drawing has unsaved changes; save it first or pass "
            "discard_changes: true");
    auto r = s.open(p.value());
    if (!r)
        return ToolResult::fail(r.error().message);
    return ToolResult::ok(drawingInfoJson(s, ctx.serverVersion));
}

ToolResult newDrawing(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    if (s.file().modified && !argBool(a, "discard_changes"))
        return ToolResult::fail(
            "the open drawing has unsaved changes; save it first or pass "
            "discard_changes: true");
    s.newDocument();
    return ToolResult::ok(drawingInfoJson(s, ctx.serverVersion));
}

ToolResult setView(ToolContext& ctx, const Json& a) {
    DrawingSession& s = ctx.session;
    auto v = s.view();
    if (!v)
        return ToolResult::fail("this session has no viewport");
    if (const Json* b = a.find("bbox")) {
        if (!b->isArray() || b->asArray().size() != 2)
            return ToolResult::fail("bbox must be [[x1, y1], [x2, y2]]");
        auto p1 = pointFromJson(b->asArray()[0]);
        auto p2 = pointFromJson(b->asArray()[1]);
        if (!p1 || !p2)
            return ToolResult::fail("bbox must be [[x1, y1], [x2, y2]]");
        BBox2 box;
        box.expand(p1.value());
        box.expand(p2.value());
        s.zoomToBox(box);
    } else if (const Json* c = a.find("center")) {
        auto p = pointFromJson(*c);
        if (!p)
            return ToolResult::fail("center must be [x, y]");
        ViewInfo nv = *v;
        nv.center = p.value();
        double mpp = argNumber(a, "mm_per_pixel", v->mmPerPixel);
        if (!(mpp > 0))
            return ToolResult::fail("mm_per_pixel must be positive");
        nv.mmPerPixel = mpp;
        s.setView(nv);
    } else {
        BBox2 ext = drawingExtents(s);
        if (ext.empty())
            return ToolResult::fail("the drawing is empty");
        s.zoomToBox(ext);
    }
    auto nv = s.view();
    return ToolResult::ok(
        Json(Json::Object{{"center", pointToJson(nv->center)}, {"mmPerPixel", nv->mmPerPixel}}));
}

}  // namespace

void registerFileTools(ToolSet& tools) {
    tools.add({"undo", "Undo",
               "Undoes the last AI steps (each tool call that changes the drawing is one step). "
               "Steps made by the user are left alone unless force is true.",
               tObject({{"steps", tInt("default 1, max 50", 1, 50)},
                        {"force", tBool("also undo steps the user made")}}),
               outSchema({"undone", "labels", "nextUndo"}), Edit::Session, true, false, true,
               [](ToolContext& c, const Json& a) { return stepTool(c, a, true); }});
    tools.add({"redo", "Redo", "Redoes steps that were undone.",
               tObject({{"steps", tInt("default 1, max 50", 1, 50)}}),
               outSchema({"redone", "labels", "nextRedo"}), Edit::Session, false, false, true,
               [](ToolContext& c, const Json& a) { return stepTool(c, a, false); }});

    tools.add({"save_drawing", "Save drawing",
               "Saves the drawing. Without a path it saves to the current file; with a path "
               "(absolute, ending in .dxf or .tbcad) it saves a copy there and makes it the "
               "current file.",
               tObject({{"path", tString("absolute path ending in .dxf or .tbcad")},
                        {"overwrite", tBool("replace an existing file at path")}}),
               outSchema({"saved", "path"}), Edit::Session, false, false, true, saveDrawing});
    tools.add(
        {"open_drawing", "Open drawing",
         "Opens a .dxf or .tbcad file, replacing the open drawing (refused when it has unsaved "
         "changes unless discard_changes is true).",
         tObject({{"path", tString("absolute path ending in .dxf or .tbcad")},
                  {"discard_changes", tBool()}},
                 {"path"}),
         outSchema({"name", "entityCount"}), Edit::Session, true, false, true, openDrawing});
    tools.add({"new_drawing", "New drawing",
               "Starts an empty drawing (refused when the open one has unsaved changes unless "
               "discard_changes is true).",
               tObject({{"discard_changes", tBool()}}), outSchema({"name", "entityCount"}),
               Edit::Session, true, false, true, newDrawing});

    Tool view{"set_view",
              "Set view",
              "Moves the user's viewport: fit the drawing (default), fit a bbox, or centre on a "
              "point at a scale. Not an undo step.",
              tObject({{"bbox", tArray(tPoint(), "[[x1,y1],[x2,y2]]", 2)},
                       {"center", tPoint()},
                       {"mm_per_pixel", tNumber()}}),
              outSchema({"center", "mmPerPixel"}),
              Edit::Session,
              false,
              true,
              false,
              setView};
    view.available = [](DrawingSession& s) { return s.view().has_value(); };
    tools.add(std::move(view));
}

}  // namespace tbcad::mcp::detail
