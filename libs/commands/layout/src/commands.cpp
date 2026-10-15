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

class LayoutCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LAYOUT"; }
    std::vector<std::string> defaultAliases() const override { return {"LO"}; }
    std::string_view description() const override {
        return "Create, rename, resize, delete and list layouts (sheets)";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::KeywordOptions menu;
        menu.keywords = {"New", "Delete", "Rename", "Paper", "Set", "List"};
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
            if (r)
                ctx.host().setSetting("layout.current", name.text);
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
            ctx.message("Current layout: " + named.text);
        } else if (kw == "Delete") {
            report(ctx, layout::deleteLayout(ctx.tx(), named.text),
                   "Layout \"" + named.text + "\" deleted.");
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
        auto id = co_await ctx.getInteger("Viewport number");
        if (!id.ok())
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
        report(ctx, layout::setViewportScale(ctx.tx(), target, static_cast<int>(id.value), n),
               "Viewport " + std::to_string(id.value) +
                   " is now 1:" + std::to_string(static_cast<long long>(n)));
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
        auto vp = co_await ctx.getInteger("Viewport number");
        if (!vp.ok())
            co_return;
        report(ctx,
               layout::setViewportLayerFrozen(ctx.tx(), target, static_cast<int>(vp.value),
                                              layer->id, action.keyword == "Freeze"),
               action.keyword + (action.keyword == "Freeze" ? "n " : "d ") + layer->name +
                   " in viewport " + std::to_string(vp.value) + ".");
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
        auto vp = co_await ctx.getInteger("Viewport number");
        if (!vp.ok())
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
        report(ctx, layout::setViewportLevel(ctx.tx(), target, static_cast<int>(vp.value), level),
               level.value == 0
                   ? "Viewport " + std::to_string(vp.value) + " shows all levels."
                   : "Viewport " + std::to_string(vp.value) + " shows only " + name.text + ".");
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
    (void)registry.add(std::make_unique<VpLayerCommand>());
    (void)registry.add(std::make_unique<VpLevelCommand>());
    (void)registry.add(std::make_unique<TitleBlockCommand>());
}

}  // namespace tbcad::layoutcmd
