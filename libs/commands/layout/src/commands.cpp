#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "tbcad/cmd/cmd.h"
#include "tbcad/geom/entity.h"
#include "tbcad/layout/layout.h"
#include "tbcad/layoutcmd/layoutcmd.h"

namespace tbcad::layoutcmd {

namespace {

using cmd::Status;

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "1:50" or "50".
bool parseScaleN(const std::string& text, double& n) {
    std::string t;
    for (char c : text)
        if (c != ' ')
            t += c;
    auto colon = t.find(':');
    if (colon != std::string::npos) {
        if (t.substr(0, colon) != "1")
            return false;
        t = t.substr(colon + 1);
    }
    char* end = nullptr;
    n = std::strtod(t.c_str(), &end);
    return !t.empty() && end && *end == 0 && n > 0;
}

// Layout to work on: the current one, or the only one.
bool requireLayout(cmd::CommandContext& ctx, std::string& name) {
    name = currentLayout(ctx.host(), ctx.doc());
    if (name.empty()) {
        ctx.message("This drawing has no layouts. Use LAYOUT > New.");
        return false;
    }
    return true;
}

void report(cmd::CommandContext& ctx, const Result<void>& r, const std::string& okText) {
    ctx.message(r ? okText : r.error().message);
}

// Which viewport: click inside it on the sheet, or type its number.
cmd::Task askViewport(cmd::CommandContext& ctx, const std::string& layoutName, int& id, bool& ok) {
    ok = false;
    auto lay = layout::findLayout(ctx.doc(), layoutName);
    if (!lay || lay->viewports.empty()) {
        ctx.message("\"" + layoutName + "\" has no viewports. Use MVIEW to add one.");
        co_return;
    }
    // A viewport selected on the sheet by clicking its border is the answer.
    if (const int chosen = selectedViewport(ctx.host(), ctx.doc())) {
        id = chosen;
        ok = true;
        ctx.message("Using the selected viewport " + std::to_string(chosen) + ".");
        co_return;
    }
    cmd::PointOptions po;
    po.allowNone = false;
    for (const layout::Viewport& v : lay->viewports)
        po.keywords.push_back(std::to_string(v.id));
    auto r = co_await ctx.getPoint("Click inside a viewport, or type its number", po);
    if (r.status == Status::Keyword) {
        id = std::atoi(r.keyword.c_str());
        ok = true;
    } else if (r.ok()) {
        for (const layout::Viewport& v : lay->viewports)  // the last one is drawn on top
            if (layout::viewportContains(v, r.point)) {
                id = v.id;
                ok = true;
            }
        if (!ok)
            ctx.message("No viewport there.");
    }
}

class LayoutCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYOUT"; }
    std::vector<std::string> defaultAliases() const override { return {"LO"}; }
    std::string_view description() const override {
        return "Create, rename, resize, delete and list layouts (sheets)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions menu;
        menu.keywords = {"New", "Copy", "Delete", "Rename", "Paper", "Set", "List"};
        menu.defaultKeyword = "List";
        auto choice = co_await ctx.getKeyword("Layout", menu);
        if (choice.status == Status::Cancel)
            co_return;
        const std::string& kw = choice.keyword;

        if (kw == "List") {
            auto all = layout::layouts(ctx.doc());
            if (all.empty())
                ctx.message("No layouts.");
            const std::string cur = currentLayout(ctx.host(), ctx.doc());
            for (const auto& l : all)
                ctx.message((lower(l.name) == lower(cur) ? "* " : "  ") + l.name + "  " +
                            l.paperName + (l.landscape ? " landscape, " : " portrait, ") +
                            std::to_string(l.viewports.size()) + " viewports");
            co_return;
        }
        if (kw == "New") {
            auto name = co_await ctx.getString("Layout name");
            if (!name.ok())
                co_return;
            cmd::StringOptions po;
            po.defaultValue = "A3";
            auto paper = co_await ctx.getString("Paper size (A0-A4, ANSI A-E)", po);
            if (!paper.ok())
                co_return;
            std::string paperName = paper.text;
            for (const auto& p : exportlib::paperSizes())
                if (lower(p.name) == lower(paper.text))
                    paperName = p.name;
            cmd::KeywordOptions ko;
            ko.keywords = {"Landscape", "Portrait"};
            ko.defaultKeyword = "Landscape";
            auto orient = co_await ctx.getKeyword("Orientation", ko);
            if (!orient.ok())
                co_return;
            auto r =
                layout::createLayout(ctx.tx(), name.text, paperName, orient.keyword == "Landscape");
            report(ctx, r, "Layout \"" + name.text + "\" created.");
            if (r) {
                ctx.host().setSetting("layout.current", name.text);
                ctx.host().setSetting("layout.show", name.text);  // the window follows
            }
            co_return;
        }
        // The rest work on a named layout, defaulting to the current one.
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        cmd::StringOptions lo;
        lo.defaultValue = target;
        auto named = co_await ctx.getString("Layout name", lo);
        if (!named.ok())
            co_return;
        if (!layout::findLayout(ctx.doc(), named.text)) {
            ctx.message("No layout named \"" + named.text + "\".");
            co_return;
        }
        if (kw == "Set") {
            ctx.host().setSetting("layout.current",
                                  layout::findLayout(ctx.doc(), named.text)->name);
            ctx.host().setSetting("layout.show", named.text);
            ctx.message("Current layout: " + named.text);
        } else if (kw == "Delete") {
            report(ctx, layout::deleteLayout(ctx.tx(), named.text),
                   "Layout \"" + named.text + "\" deleted.");
        } else if (kw == "Copy") {
            cmd::StringOptions co;
            co.defaultValue = named.text + " (2)";
            auto nn = co_await ctx.getString("Name of the copy", co);
            if (!nn.ok())
                co_return;
            auto r = layout::copyLayout(ctx.tx(), named.text, nn.text);
            report(ctx, r, "Layout \"" + named.text + "\" copied to \"" + nn.text + "\".");
            if (r) {
                ctx.host().setSetting("layout.current", nn.text);
                ctx.host().setSetting("layout.show", nn.text);
            }
        } else if (kw == "Rename") {
            auto nn = co_await ctx.getString("New name");
            if (!nn.ok())
                co_return;
            auto r = layout::renameLayout(ctx.tx(), named.text, nn.text);
            report(ctx, r, "Renamed to \"" + nn.text + "\".");
            if (r && lower(currentLayout(ctx.host(), ctx.doc())) == lower(named.text))
                ctx.host().setSetting("layout.current", nn.text);
        } else if (kw == "Paper") {
            cmd::StringOptions po;
            po.defaultValue = layout::findLayout(ctx.doc(), named.text)->paperName;
            auto paper = co_await ctx.getString("Paper size (A0-A4, ANSI A-E, Custom)", po);
            if (!paper.ok())
                co_return;
            double w = 0, h = 0;
            std::string paperName = paper.text;
            if (lower(paper.text) == "custom") {
                paperName = "Custom";
                auto rw = co_await ctx.getReal("Paper width in mm");
                if (!rw.ok())
                    co_return;
                auto rh = co_await ctx.getReal("Paper height in mm");
                if (!rh.ok())
                    co_return;
                w = rw.value;
                h = rh.value;
            } else {
                for (const auto& p : exportlib::paperSizes())
                    if (lower(p.name) == lower(paper.text))
                        paperName = p.name;
            }
            cmd::KeywordOptions ko;
            ko.keywords = {"Landscape", "Portrait"};
            ko.defaultKeyword =
                layout::findLayout(ctx.doc(), named.text)->landscape ? "Landscape" : "Portrait";
            auto orient = co_await ctx.getKeyword("Orientation", ko);
            if (!orient.ok())
                co_return;
            report(ctx,
                   layout::setLayoutPaper(ctx.tx(), named.text, paperName,
                                          orient.keyword == "Landscape", w, h),
                   "Paper changed.");
        }
    }
};

// MVIEW: a viewport on the current layout. Corners are typed on the sheet in millimetres.
class MviewCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MVIEW"; }
    std::vector<std::string> defaultAliases() const override { return {"MV"}; }
    std::string_view description() const override {
        return "Add a viewport to the current layout (sheet millimetres)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        auto a = co_await ctx.getPoint("Specify first corner on the sheet (mm)");
        if (!a.ok())
            co_return;
        cmd::PointOptions po;
        po.base = a.point;
        auto b = co_await ctx.getPoint("Specify opposite corner on the sheet (mm)", po);
        if (!b.ok())
            co_return;
        cmd::StringOptions so;
        so.defaultValue = "1:100";
        auto scale = co_await ctx.getString("Viewport scale (1:N)", so);
        if (!scale.ok())
            co_return;
        double n = 0;
        if (!parseScaleN(scale.text, n)) {
            ctx.message("Not a scale: \"" + scale.text + "\". Use 1:N.");
            co_return;
        }
        cmd::PointOptions co;
        auto center = co_await ctx.getPoint(
            "Specify the model point at the viewport centre <centre of the drawing>", co);
        Vec2 centre{};
        if (center.status == Status::None) {  // Enter: show the middle of the drawing
            BBox2 box;
            for (EntityId id : ctx.doc().modelSpace())
                if (const Entity* e = ctx.doc().find(id))
                    box.merge(geom::bounds(*e, ctx.doc()));
            if (box.empty()) {
                ctx.message("The drawing is empty: type the model point.");
                co_return;
            }
            centre = {(box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2};
        } else if (center.ok()) {
            centre = center.point;
        } else {
            co_return;
        }
        BBox2 rect;
        rect.expand(a.point);
        rect.expand(b.point);
        auto id = layout::addViewport(ctx.tx(), target, rect, centre, n);
        ctx.message(id ? "Viewport " + std::to_string(id.value()) + " added to \"" + target + "\"."
                       : id.error().message);
    }
};

class VpScaleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPSCALE"; }
    std::string_view description() const override { return "Set the scale of a viewport"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        cmd::StringOptions so;
        so.defaultValue = "1:100";
        auto scale = co_await ctx.getString("New scale (1:N)", so);
        if (!scale.ok())
            co_return;
        double n = 0;
        if (!parseScaleN(scale.text, n)) {
            ctx.message("Not a scale: \"" + scale.text + "\". Use 1:N.");
            co_return;
        }
        report(ctx, layout::setViewportScale(ctx.tx(), target, vpId, n),
               "Viewport " + std::to_string(vpId) +
                   " is now 1:" + std::to_string(static_cast<long long>(n)));
    }
};

// VPLOCK: stop a viewport's scale and view from changing by accident.
class VpLockCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPLOCK"; }
    std::string_view description() const override {
        return "Lock or unlock the scale and view of a viewport";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        // A viewport already selected on the sheet is locked straight away; otherwise ask which
        // one first, then whether to lock or unlock it.
        const bool preselected = selectedViewport(ctx.host(), ctx.doc()) != 0;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        bool on = true;
        if (!preselected) {
            bool locked = false;
            if (auto lay = layout::findLayout(ctx.doc(), target))
                for (const layout::Viewport& v : lay->viewports)
                    if (v.id == vpId)
                        locked = v.locked;
            cmd::KeywordOptions ko;
            ko.keywords = {"Lock", "Unlock"};
            ko.defaultKeyword = locked ? "Unlock" : "Lock";
            auto mode = co_await ctx.getKeyword(
                "Viewport " + std::to_string(vpId) + " [Lock/Unlock] <" + *ko.defaultKeyword + ">",
                ko);
            if (!mode.ok())
                co_return;
            on = mode.keyword == "Lock";
        }
        report(ctx, layout::setViewportLocked(ctx.tx(), target, vpId, on),
               "Viewport " + std::to_string(vpId) + (on ? " is locked." : " is unlocked."));
    }
};

// VPLAYER: freeze or thaw a layer in one viewport only.
class VpLayerCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPLAYER"; }
    std::string_view description() const override {
        return "Freeze or thaw a layer in one viewport of the current layout";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Freeze", "Thaw"};
        ko.defaultKeyword = "Freeze";
        auto action = co_await ctx.getKeyword("Viewport layer", ko);
        if (!action.ok())
            co_return;
        auto layerName = co_await ctx.getString("Layer name");
        if (!layerName.ok())
            co_return;
        const Layer* layer = nullptr;
        for (const auto& [id, l] : ctx.doc().layers())
            if (lower(l.name) == lower(layerName.text) && !layer)
                layer = &l;
        if (!layer) {
            ctx.message("No layer named \"" + layerName.text +
                        "\" (sub-layers: use the layer panel).");
            co_return;
        }
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        report(ctx,
               layout::setViewportLayerFrozen(ctx.tx(), target, vpId, layer->id,
                                              action.keyword == "Freeze"),
               action.keyword + (action.keyword == "Freeze" ? "n " : "d ") + layer->name +
                   " in viewport " + std::to_string(vpId) + ".");
    }
};

// VPLEVEL: show one level only in one viewport.
class VpLevelCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPLEVEL"; }
    std::string_view description() const override {
        return "Show one level only (or all levels) in a viewport of the current layout";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        cmd::StringOptions so;
        so.allowEmpty = true;
        auto name = co_await ctx.getString("Level name (empty = all levels)", so);
        if (!name.ok())
            co_return;
        LevelId level{};
        if (!name.text.empty()) {
            const Level* found = nullptr;
            for (const auto& [id, l] : ctx.doc().levels())
                if (lower(l.name) == lower(name.text) && !found)
                    found = &l;
            if (!found) {
                ctx.message("No level named \"" + name.text + "\".");
                co_return;
            }
            level = found->id;
        }
        report(ctx, layout::setViewportLevel(ctx.tx(), target, vpId, level),
               level.value == 0
                   ? "Viewport " + std::to_string(vpId) + " shows all levels."
                   : "Viewport " + std::to_string(vpId) + " shows only " + name.text + ".");
    }
};

// VPDELETE, VPMOVE, VPRESIZE, VPCENTER: the rest of what a drafter does to a viewport.
class VpDeleteCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPDELETE"; }
    std::string_view description() const override {
        return "Delete a viewport of the current layout";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        report(ctx, layout::removeViewport(ctx.tx(), target, vpId),
               "Viewport " + std::to_string(vpId) + " deleted.");
    }
};

class VpMoveCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPMOVE"; }
    std::string_view description() const override { return "Move a viewport on the sheet"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        const auto lay = layout::findLayout(ctx.doc(), target);
        const layout::Viewport* vp = lay ? lay->findViewport(vpId) : nullptr;
        if (!vp) {
            ctx.message("No viewport " + std::to_string(vpId) + ".");
            co_return;
        }
        const BBox2 rect = vp->paper;
        auto from = co_await ctx.getPoint("Specify base point (sheet mm)");
        if (!from.ok())
            co_return;
        cmd::PointOptions po;
        po.base = from.point;
        po.preview = [rect, from = from.point](Vec2 to, DisplayList& out) {
            const Vec2 d = to - from;
            DisplayPolyline pl;
            pl.closed = true;
            pl.pts = {rect.min + d,
                      {rect.max.x + d.x, rect.min.y + d.y},
                      rect.max + d,
                      {rect.min.x + d.x, rect.max.y + d.y}};
            out.items.emplace_back(std::move(pl));
        };
        auto to = co_await ctx.getPoint("Specify second point (sheet mm)", po);
        if (!to.ok())
            co_return;
        const Vec2 d = to.point - from.point;
        BBox2 moved;
        moved.expand(rect.min + d);
        moved.expand(rect.max + d);
        report(ctx, layout::setViewportRect(ctx.tx(), target, vpId, moved),
               "Viewport " + std::to_string(vpId) + " moved.");
    }
};

// CHSPACE [CS]: moves objects between model space and the current sheet. In model space the
// objects go onto the sheet, at the size they appear in the chosen viewport; on the sheet they go
// back into model space.
class ChSpaceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CHSPACE"; }
    std::vector<std::string> defaultAliases() const override { return {"CS"}; }
    std::string_view description() const override {
        return "Move objects between model space and the sheet";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        const BlockId owner = ctx.host().drawingOwner();
        const bool toModel = static_cast<bool>(owner);
        cmd::SelectionOptions so;
        so.filter = [&doc = ctx.doc(), owner](const Entity& e) {
            return doc.ownerBlock(e.common.id) == owner;
        };
        auto sel = co_await ctx.getSelection(toModel ? "Select objects to move into model space"
                                                     : "Select objects to move to the sheet",
                                             so);
        if (!sel.ok() || sel.ids.empty())
            co_return;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        const auto lay = layout::findLayout(ctx.doc(), target);
        const layout::Viewport* vp = lay ? lay->findViewport(vpId) : nullptr;
        if (!vp) {
            ctx.message("No viewport " + std::to_string(vpId) + ".");
            co_return;
        }
        // sheet mm -> model units: the viewport centre sits at the middle of its rectangle.
        const Vec2 mid = (vp->paper.min + vp->paper.max) * 0.5;
        const Transform2 sheetToModel = Transform2::translate(vp->center) *
                                        Transform2::scale(vp->scaleN, vp->scaleN) *
                                        Transform2::translate(mid * -1.0);
        Transform2 t = sheetToModel;
        if (!toModel) {
            auto inv = sheetToModel.inverse();
            if (!inv) {
                ctx.message(inv.error().message);
                co_return;
            }
            t = inv.value();
        }
        const BlockId dest = toModel ? BlockId{} : lay->content;
        std::size_t moved = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e)
                continue;
            auto copy = geom::transformed(*e, t);
            if (!copy) {
                ctx.message(copy.error().message);
                continue;
            }
            Entity n = std::move(copy.value());
            if (!ctx.tx().erase(id))
                continue;
            ctx.tx().setDefaultOwner(dest);
            if (auto added = ctx.tx().add(std::move(n), dest); !added) {
                ctx.message(added.error().message);
                co_return;
            }
            ++moved;
        }
        ctx.setUndoLabel("CHSPACE");
        ctx.message(std::to_string(moved) + (toModel ? " objects moved into model space."
                                                     : " objects moved to the sheet."));
    }
};

// VPCLIP [VC]: gives a viewport an outline of any shape, from a closed polyline on the sheet or
// from points picked one after another, or turns it back into a rectangle.
class VpClipCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPCLIP"; }
    std::vector<std::string> defaultAliases() const override { return {"VC"}; }
    std::string_view description() const override {
        return "Clip a viewport to a polygon or a closed polyline, or make it a rectangle again";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        cmd::KeywordOptions ko;
        ko.keywords = {"Object", "Polygonal", "Delete"};
        ko.defaultKeyword = "Polygonal";
        auto mode = co_await ctx.getKeyword(
            "Select clipping object or [Object/Polygonal/Delete] <Polygonal>", ko);
        if (!mode.ok())
            co_return;
        std::vector<Vec2> outline;
        if (mode.keyword == "Delete") {
            report(ctx, layout::setViewportClip(ctx.tx(), target, vpId, {}),
                   "Viewport " + std::to_string(vpId) + " is a rectangle again.");
            co_return;
        }
        if (mode.keyword == "Object") {
            cmd::EntityOptions eo;
            eo.filter = [](const Entity& e) {
                const auto* pl = entityAs<PolylineData>(e);
                return pl && pl->closed && pl->v.size() >= 3;
            };
            auto o = co_await ctx.getEntity("Select a closed polyline on the sheet", eo);
            if (!o.ok())
                co_return;
            const Entity* e = ctx.doc().find(o.id);
            const auto* pl = e ? entityAs<PolylineData>(*e) : nullptr;
            if (!pl)
                co_return;
            for (const PolyVertex& v : pl->v)
                outline.push_back(v.p);
        } else {
            for (;;) {
                cmd::PointOptions po;
                if (!outline.empty())
                    po.base = outline.back();
                if (outline.size() >= 3)
                    po.keywords = {"Close"};
                po.preview = [&outline](Vec2 cursor, DisplayList& out) {
                    DisplayPolyline pl;
                    pl.pts = outline;
                    pl.pts.push_back(cursor);
                    if (pl.pts.size() >= 3)
                        pl.closed = true;
                    if (pl.pts.size() >= 2)
                        out.items.emplace_back(std::move(pl));
                };
                po.allowNone = outline.size() >= 3;
                const std::string prompt =
                    outline.size() < 3
                        ? "Specify point " + std::to_string(outline.size() + 1) +
                              " of the outline on the sheet (mm)"
                        : "Specify point " + std::to_string(outline.size() + 1) +
                              " or [Close] <accept " + std::to_string(outline.size()) + " points>";
                auto p = co_await ctx.getPoint(prompt, po);
                if (p.status == Status::Keyword || p.status == Status::None)
                    break;
                if (!p.ok())
                    co_return;
                outline.push_back(p.point);
            }
        }
        if (outline.size() < 3) {
            ctx.message("A clip needs at least three points.");
            co_return;
        }
        ctx.setUndoLabel("VPCLIP");
        report(ctx, layout::setViewportClip(ctx.tx(), target, vpId, outline),
               "Viewport " + std::to_string(vpId) + " is clipped to the outline.");
    }
};

class VpResizeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPRESIZE"; }
    std::string_view description() const override {
        return "Give a viewport new corners on the sheet";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        auto a = co_await ctx.getPoint("Specify first corner on the sheet (mm)");
        if (!a.ok())
            co_return;
        cmd::PointOptions po;
        po.base = a.point;
        po.preview = [first = a.point](Vec2 c, DisplayList& out) {
            DisplayPolyline pl;
            pl.closed = true;
            pl.pts = {first, {c.x, first.y}, c, {first.x, c.y}};
            out.items.emplace_back(std::move(pl));
        };
        auto b = co_await ctx.getPoint("Specify opposite corner on the sheet (mm)", po);
        if (!b.ok())
            co_return;
        BBox2 rect;
        rect.expand(a.point);
        rect.expand(b.point);
        report(ctx, layout::setViewportRect(ctx.tx(), target, vpId, rect),
               "Viewport " + std::to_string(vpId) + " resized.");
    }
};

class VpCenterCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "VPCENTER"; }
    std::string_view description() const override {
        return "Change which model point a viewport shows at its middle";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        int vpId = 0;
        bool picked = false;
        co_await askViewport(ctx, target, vpId, picked);
        if (!picked)
            co_return;
        auto c = co_await ctx.getPoint("Specify the model point at the viewport centre");
        if (!c.ok())
            co_return;
        report(ctx, layout::setViewportCenter(ctx.tx(), target, vpId, c.point),
               "Viewport " + std::to_string(vpId) + " centred.");
    }
};

class TitleBlockCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TITLEBLOCK"; }
    std::string_view description() const override {
        return "Add a title block to the current layout";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        layout::TitleBlockFields f;
        cmd::StringOptions so;
        so.allowEmpty = true;
        auto project = co_await ctx.getString("Project", so);
        if (!project.ok())
            co_return;
        auto drawing = co_await ctx.getString("Drawing", so);
        if (!drawing.ok())
            co_return;
        auto scale = co_await ctx.getString("Scale", so);
        if (!scale.ok())
            co_return;
        auto sheet = co_await ctx.getString("Sheet number", so);
        if (!sheet.ok())
            co_return;
        auto date = co_await ctx.getString("Date", so);
        if (!date.ok())
            co_return;
        f.project = project.text;
        f.drawing = drawing.text;
        f.scale = scale.text;
        f.sheet = sheet.text;
        f.date = date.text;
        report(ctx, layout::addTitleBlock(ctx.tx(), target, f),
               "Title block added to \"" + target + "\".");
    }
};

}  // namespace

std::string currentLayout(const cmd::IHost& host, const Document& doc) {
    if (auto cur = host.setting("layout.current"); cur && !cur->empty())
        if (auto l = layout::findLayout(doc, *cur))
            return l->name;
    auto all = layout::layouts(doc);
    return all.empty() ? std::string() : all.front().name;
}

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<LayoutCommand>());
    (void)registry.add(std::make_unique<MviewCommand>());
    (void)registry.add(std::make_unique<VpScaleCommand>());
    (void)registry.add(std::make_unique<VpLockCommand>());
    (void)registry.add(std::make_unique<VpLayerCommand>());
    (void)registry.add(std::make_unique<VpLevelCommand>());
    (void)registry.add(std::make_unique<VpDeleteCommand>());
    (void)registry.add(std::make_unique<VpMoveCommand>());
    (void)registry.add(std::make_unique<VpResizeCommand>());
    (void)registry.add(std::make_unique<VpClipCommand>());
    (void)registry.add(std::make_unique<VpCenterCommand>());
    (void)registry.add(std::make_unique<ChSpaceCommand>());
    (void)registry.add(std::make_unique<TitleBlockCommand>());
}

}  // namespace tbcad::layoutcmd
