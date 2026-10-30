#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>

#include "tbcad/clipboard/clipboard.h"
#include "tbcad/cmd/cmd.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/dxf/writer.h"
#include "tbcad/geom/entity.h"
#include "tbcad/io/tbcad.h"
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
        menu.keywords = {"New",   "Copy", "Delete", "Rename",   "Move",
                         "Paper", "Set",  "List",   "Template", "SAveas"};
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
        if (kw == "Template") {
            co_await fromTemplate(ctx);
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
        } else if (kw == "Move") {
            const auto all = layout::layouts(ctx.doc());
            std::size_t at = 0;
            for (std::size_t i = 0; i < all.size(); ++i)
                if (lower(all[i].name) == lower(named.text))
                    at = i;
            cmd::IntOptions io;
            io.defaultValue = static_cast<long long>(at) + 1;
            auto pos = co_await ctx.getInteger(
                "Place in the tab order (1-" + std::to_string(all.size()) + ")", io);
            if (!pos.ok())
                co_return;
            report(
                ctx,
                layout::moveLayout(ctx.tx(), named.text,
                                   static_cast<std::size_t>(std::max<long long>(1, pos.value) - 1)),
                "Layout \"" + named.text + "\" moved.");
        } else if (kw == "SAveas") {
            // A drawing that holds only this layout (its sheet objects and viewports, with the
            // layers and styles of the drawing) and no model-space objects; Template reads it back.
            cmd::StringOptions fo;
            fo.defaultValue = named.text + ".tbcad";
            auto f = co_await ctx.getString("File to save the layout template to (.tbcad)", fo);
            if (!f.ok())
                co_return;
            std::string path = cmd::trim(f.text);
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                path = path.substr(1, path.size() - 2);
            if (path.empty())
                co_return;
            auto bytes = io::writeTbcadToMemory(ctx.doc());
            if (!bytes) {
                ctx.message(bytes.error().message);
                co_return;
            }
            io::IoReport rep;
            auto copy = io::readTbcad(std::span<const std::uint8_t>(bytes.value()), rep);
            if (!copy) {
                ctx.message(copy.error().message);
                co_return;
            }
            Document& tpl = *copy.value();
            {
                Transaction tx = tpl.begin("Layout template");
                for (const auto& l : layout::layouts(tpl))
                    if (lower(l.name) != lower(named.text))
                        (void)layout::deleteLayout(tx, l.name);
                for (EntityId id : tpl.modelSpace())
                    (void)tx.erase(id);
                tx.commit();
            }
            std::filesystem::path file(std::u8string(path.begin(), path.end()));
            if (file.extension().empty())
                file += ".tbcad";
            auto w = io::writeTbcad(tpl, file);
            ctx.message(w ? "Layout \"" + named.text + "\" saved to " + file.string() + "."
                          : "Cannot save: " + w.error().message);
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

private:
    // The layer of `dst` with the same name path as `layer` of `src`, created when missing.
    static LayerId matchLayer(cmd::CommandContext& ctx, const Document& src, LayerId layer) {
        std::vector<const Layer*> chain;  // the layer first, then its parents
        for (const Layer* l = src.layer(layer); l && chain.size() < 16;
             l = l->parent.value ? src.layer(l->parent) : nullptr)
            chain.push_back(l);
        if (chain.empty())
            return {};
        LayerId parent{};
        for (std::size_t i = chain.size(); i-- > 0;) {
            const Layer* have = parent.value ? ctx.doc().layerByName(parent, chain[i]->name)
                                             : ctx.doc().layerByName(chain[i]->name);
            if (!have) {
                Layer row;
                row.name = chain[i]->name;
                row.parent = parent;
                row.color = chain[i]->color;
                row.description = chain[i]->description;
                auto made = ctx.tx().addLayer(row);
                if (!made)
                    return {};
                parent = made.value();
            } else {
                parent = have->id;
            }
        }
        return parent;
    }

    // LAYOUT Template: brings a layout (paper, sheet objects and viewports) from a drawing file
    // (.tbcad or .dxf) into this drawing. Layers, styles and blocks the sheet uses are matched by
    // name and created when missing; layers frozen per viewport are matched by name (path for
    // sub-layers) and created when missing.
    static cmd::Task fromTemplate(cmd::CommandContext& ctx) {
        auto f = co_await ctx.getString("Drawing file with the layout (.tbcad or .dxf)");
        if (!f.ok())
            co_return;
        std::string path = cmd::trim(f.text);
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        const std::filesystem::path file(std::u8string(path.begin(), path.end()));
        std::unique_ptr<Document> src;
        std::string ext = lower(file.extension().string());
        if (ext == ".dxf") {
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                ctx.message("Cannot read " + path);
                co_return;
            }
            std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            dxf::Report rep;
            auto r = dxf::importDxf(std::span<const std::byte>(
                                        reinterpret_cast<const std::byte*>(raw.data()), raw.size()),
                                    dxf::ImportOptions{}, rep);
            if (!r) {
                ctx.message("Cannot read " + path + ": " + r.error().message);
                co_return;
            }
            src = std::move(r.value().document);
        } else {
            io::IoReport rep;
            auto r = io::readTbcad(file, rep);
            if (!r) {
                ctx.message("Cannot read " + path + ": " + r.error().message);
                co_return;
            }
            src = std::move(r.value());
        }
        const auto all = layout::layouts(*src);
        if (all.empty()) {
            ctx.message(path + " has no layouts.");
            co_return;
        }
        std::string pick = all.front().name;
        if (all.size() > 1) {
            std::string names;
            for (const auto& l : all)
                names += (names.empty() ? "" : ", ") + l.name;
            ctx.message("Layouts in the file: " + names);
            cmd::StringOptions po;
            po.defaultValue = pick;
            auto n = co_await ctx.getString("Layout to bring in", po);
            if (!n.ok())
                co_return;
            pick = n.text;
        }
        auto chosen = layout::findLayout(*src, pick);
        if (!chosen) {
            ctx.message("No layout named \"" + pick + "\" in the file.");
            co_return;
        }
        std::string name = chosen->name;
        for (int n = 2; layout::findLayout(ctx.doc(), name); ++n)
            name = chosen->name + " (" + std::to_string(n) + ")";
        auto made = layout::createLayout(ctx.tx(), name, chosen->paperName, chosen->landscape);
        if (!made) {
            // A custom sheet size is not a named paper: create it as A3, then set the size.
            made = layout::createLayout(ctx.tx(), name, "A3", chosen->landscape);
            if (made)
                made = layout::setLayoutPaper(ctx.tx(), name, "Custom", chosen->landscape,
                                              chosen->widthMm, chosen->heightMm);
        }
        if (!made) {
            ctx.message(made.error().message);
            co_return;
        }
        auto created = layout::findLayout(ctx.doc(), name);
        clipboard::Clip sheet = clipboard::captureBlock(*src, chosen->content);
        std::size_t objects = 0;
        if (created && !sheet.empty()) {
            clipboard::PasteOptions po;
            po.owner = created->content;
            auto ids = clipboard::paste(ctx.tx(), sheet, po);
            if (!ids) {
                ctx.message(ids.error().message);
                co_return;
            }
            objects = ids.value().size();
        }
        for (const layout::Viewport& v : chosen->viewports) {
            auto id = layout::addViewport(ctx.tx(), name, v.paper, v.center, v.scaleN);
            if (!id)
                continue;
            if (!v.clip.empty())
                (void)layout::setViewportClip(ctx.tx(), name, id.value(), v.clip);
            if (!v.on)
                (void)layout::setViewportOn(ctx.tx(), name, id.value(), false);
            if (v.locked)
                (void)layout::setViewportLocked(ctx.tx(), name, id.value(), true);
            for (LayerId fz : v.frozen) {
                const LayerId mapped = matchLayer(ctx, *src, fz);
                if (mapped.value != 0)
                    (void)layout::setViewportLayerFrozen(ctx.tx(), name, id.value(), mapped, true);
            }
        }
        ctx.host().setSetting("layout.current", name);
        ctx.host().setSetting("layout.show", name);
        ctx.message("Layout \"" + name + "\" brought in with " + std::to_string(objects) +
                    " object(s) and " + std::to_string(chosen->viewports.size()) + " viewport(s).");
    }
};

// Points of a polygon on the sheet, picked one after another (MVIEW Polygonal, VPCLIP).
// Leaves fewer than three points when cancelled.
cmd::Task pickOutline(cmd::CommandContext& ctx, std::vector<Vec2>& outline) {
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
        const std::string prompt = outline.size() < 3
                                       ? "Specify point " + std::to_string(outline.size() + 1) +
                                             " of the outline on the sheet (mm)"
                                       : "Specify point " + std::to_string(outline.size() + 1) +
                                             " or [Close] <accept " +
                                             std::to_string(outline.size()) + " points>";
        auto p = co_await ctx.getPoint(prompt, po);
        if (p.status == Status::Keyword || p.status == Status::None)
            co_return;
        if (!p.ok()) {
            outline.clear();
            co_return;
        }
        outline.push_back(p.point);
    }
}

// A closed polyline drawn on the sheet, as an outline (MVIEW Object, VPCLIP Object).
cmd::Task pickOutlineObject(cmd::CommandContext& ctx, std::vector<Vec2>& outline) {
    cmd::EntityOptions eo;
    eo.filter = [](const Entity& e) {
        const auto* pl = entityAs<PolylineData>(e);
        return pl && pl->closed && pl->v.size() >= 3;
    };
    auto o = co_await ctx.getEntity("Select a closed polyline on the sheet", eo);
    if (!o.ok())
        co_return;
    const Entity* e = ctx.doc().find(o.id);
    if (const auto* pl = e ? entityAs<PolylineData>(*e) : nullptr)
        for (const PolyVertex& v : pl->v)
            outline.push_back(v.p);
}

BBox2 modelExtents(const Document& doc) {
    BBox2 box;
    for (EntityId id : doc.modelSpace())
        if (const Entity* e = doc.find(id))
            box.merge(geom::bounds(*e, doc));
    return box;
}

// MVIEW: a viewport on the current layout. Corners are typed on the sheet in millimetres, or the
// viewport fills the sheet inside its margins (Fit), or follows a polygon (Polygonal) or a closed
// polyline on the sheet (Object). ON / OFF and Lock act on viewports already there.
class MviewCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MVIEW"; }
    std::vector<std::string> defaultAliases() const override { return {"MV"}; }
    std::string_view description() const override {
        return "Add a viewport to the current layout, or switch viewports on, off or locked";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string target;
        if (!requireLayout(ctx, target))
            co_return;
        cmd::PointOptions fo;
        fo.keywords = {"Fit", "Polygonal", "Object", "ON", "OFF", "Lock", "Layer", "2", "3", "4"};
        auto a = co_await ctx.getPoint(
            "Specify first corner on the sheet (mm) or "
            "[Fit/Polygonal/Object/ON/OFF/Lock/Layer/2/3/4]",
            fo);
        // 2 / 3 / 4: several viewports that share one area, split as asked.
        int multi = 0;
        std::string arrangement;
        if (a.status == Status::Keyword &&
            (a.keyword == "2" || a.keyword == "3" || a.keyword == "4")) {
            multi = a.keyword[0] - '0';
            if (multi < 4) {
                cmd::KeywordOptions ao;
                ao.keywords = multi == 2
                                  ? std::vector<std::string>{"Horizontal", "Vertical"}
                                  : std::vector<std::string>{"Horizontal", "Vertical", "Above",
                                                             "Below",      "Left",     "Right"};
                ao.defaultKeyword = multi == 2 ? "Vertical" : "Right";
                auto k = co_await ctx.getKeyword(
                    multi == 2 ? "Arrangement [Horizontal/Vertical] <Vertical>"
                               : "Arrangement [Horizontal/Vertical/Above/Below/Left/Right] <Right>",
                    ao);
                if (!k.ok())
                    co_return;
                arrangement = k.keyword;
            }
            cmd::PointOptions po;
            po.keywords = {"Fit"};
            a = co_await ctx.getPoint("Specify first corner on the sheet (mm) or [Fit] <Fit>", po);
            if (a.status == Status::None)
                a.status = Status::Keyword, a.keyword = "Fit";
        }
        if (a.status == Status::Keyword && (a.keyword == "ON" || a.keyword == "OFF")) {
            int vpId = 0;
            bool picked = false;
            co_await askViewport(ctx, target, vpId, picked);
            if (picked)
                report(ctx, layout::setViewportOn(ctx.tx(), target, vpId, a.keyword == "ON"),
                       "Viewport " + std::to_string(vpId) +
                           (a.keyword == "ON" ? " is on." : " is off."));
            co_return;
        }
        if (a.status == Status::Keyword && a.keyword == "Layer") {
            // The viewport border goes on this layer: its colour, and off / frozen / no-plot.
            auto layerName = co_await ctx.getString("Layer name");
            if (!layerName.ok())
                co_return;
            const Layer* layer = nullptr;
            for (const auto& [id, l] : ctx.doc().layers())
                if (lower(l.name) == lower(layerName.text) && !layer)
                    layer = &l;
            if (!layer) {
                ctx.message("No layer named \"" + layerName.text + "\".");
                co_return;
            }
            const LayerId layerId = layer->id;
            int vpId = 0;
            bool picked = false;
            co_await askViewport(ctx, target, vpId, picked);
            if (picked)
                report(ctx, layout::setViewportBorderLayer(ctx.tx(), target, vpId, layerId),
                       "Viewport " + std::to_string(vpId) + " is on layer " + layer->name + ".");
            co_return;
        }
        if (a.status == Status::Keyword && a.keyword == "Lock") {
            cmd::KeywordOptions ko;
            ko.keywords = {"ON", "OFF"};
            ko.defaultKeyword = "ON";
            auto mode = co_await ctx.getKeyword("Viewport view locking [ON/OFF] <ON>", ko);
            if (!mode.ok())
                co_return;
            int vpId = 0;
            bool picked = false;
            co_await askViewport(ctx, target, vpId, picked);
            if (picked) {
                const bool on = mode.keyword == "ON";
                report(ctx, layout::setViewportLocked(ctx.tx(), target, vpId, on),
                       "Viewport " + std::to_string(vpId) + (on ? " is locked." : " is unlocked."));
            }
            co_return;
        }
        BBox2 rect;
        std::vector<Vec2> outline;
        if (a.status == Status::Keyword && a.keyword == "Fit") {
            const auto lay = layout::findLayout(ctx.doc(), target);
            if (!lay)
                co_return;
            const double m = std::min({kFitMarginMm, lay->widthMm / 4, lay->heightMm / 4});
            rect.expand({m, m});
            rect.expand({lay->widthMm - m, lay->heightMm - m});
        } else if (a.status == Status::Keyword) {
            if (a.keyword == "Object")
                co_await pickOutlineObject(ctx, outline);
            else
                co_await pickOutline(ctx, outline);
            if (outline.size() < 3) {
                ctx.message("A viewport outline needs at least three points.");
                co_return;
            }
            for (Vec2 q : outline)
                rect.expand(q);
        } else if (a.ok()) {
            cmd::PointOptions po;
            po.base = a.point;
            auto b = co_await ctx.getPoint("Specify opposite corner on the sheet (mm)", po);
            if (!b.ok())
                co_return;
            rect.expand(a.point);
            rect.expand(b.point);
        } else {
            co_return;
        }

        std::vector<BBox2> rects{rect};
        if (multi) {
            rects = splitViewportArea(rect, multi, arrangement);
            if (!outline.empty())
                outline.clear();  // the outline only makes sense for a single viewport
        }
        // The scale: typed, or Fit to show the whole drawing.
        cmd::StringOptions so;
        so.defaultValue = "1:100";
        double typedN = 0;
        Vec2 typedCentre{};
        bool fit = false;
        for (;;) {  // a wrong answer asks again: the corners picked so far are not thrown away
            auto scale = co_await ctx.getString("Viewport scale (1:N) or [Fit] <1:100>", so);
            if (!scale.ok())
                co_return;
            fit = lower(scale.text) == "fit" || lower(scale.text) == "f";
            if (fit || parseScaleN(scale.text, typedN))
                break;
            ctx.message("Not a scale: \"" + scale.text + "\". Use 1:N.");
        }
        const BBox2 box = modelExtents(ctx.doc());
        if (fit) {
            if (box.empty()) {
                ctx.message("The drawing is empty: type a scale.");
                co_return;
            }
        } else {
            cmd::PointOptions co;
            auto center = co_await ctx.getPoint(
                "Specify the model point at the viewport centre <centre of the drawing>", co);
            if (center.status == Status::None) {  // Enter: show the middle of the drawing
                if (box.empty()) {
                    ctx.message("The drawing is empty: type the model point.");
                    co_return;
                }
                typedCentre = {(box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2};
            } else if (center.ok()) {
                typedCentre = center.point;
            } else {
                co_return;
            }
        }
        for (const BBox2& r : rects) {
            double n = typedN;
            Vec2 centre = typedCentre;
            if (fit) {
                if (!(r.width() > 0) || !(r.height() > 0)) {
                    ctx.message("The drawing is empty: type a scale.");
                    co_return;
                }
                n = std::max(box.width() / r.width(), box.height() / r.height());
                if (!(n > 0))
                    n = 1;
                centre = {(box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2};
            }
            auto id = layout::addViewport(ctx.tx(), target, r, centre, n);
            if (id && !outline.empty())
                if (auto res = layout::setViewportClip(ctx.tx(), target, id.value(), outline); !res)
                    ctx.message(res.error().message);
            ctx.message(id ? "Viewport " + std::to_string(id.value()) + " added to \"" + target +
                                 "\"" + (fit ? " at 1:" + formatScale(n) : std::string()) + "."
                           : id.error().message);
        }
    }

private:
    // The area cut into `count` viewports (2, 3 or 4) the way AutoCAD's MVIEW does: halves,
    // a big one beside two small ones, or a 2 x 2 grid.
    static std::vector<BBox2> splitViewportArea(const BBox2& r, int count, const std::string& how) {
        const double x0 = r.min.x, x1 = r.max.x, y0 = r.min.y, y1 = r.max.y;
        const double xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
        auto box = [](double ax, double ay, double bx, double by) {
            BBox2 b;
            b.expand({ax, ay});
            b.expand({bx, by});
            return b;
        };
        if (count == 4)
            return {box(x0, y0, xm, ym), box(xm, y0, x1, ym), box(x0, ym, xm, y1),
                    box(xm, ym, x1, y1)};
        if (count == 2) {
            if (how == "Horizontal")  // one above the other
                return {box(x0, y0, x1, ym), box(x0, ym, x1, y1)};
            return {box(x0, y0, xm, y1), box(xm, y0, x1, y1)};
        }
        if (how == "Horizontal")  // three rows
            return {box(x0, y0, x1, y0 + (y1 - y0) / 3),
                    box(x0, y0 + (y1 - y0) / 3, x1, y0 + 2 * (y1 - y0) / 3),
                    box(x0, y0 + 2 * (y1 - y0) / 3, x1, y1)};
        if (how == "Vertical")  // three columns
            return {box(x0, y0, x0 + (x1 - x0) / 3, y1),
                    box(x0 + (x1 - x0) / 3, y0, x0 + 2 * (x1 - x0) / 3, y1),
                    box(x0 + 2 * (x1 - x0) / 3, y0, x1, y1)};
        if (how == "Above")  // the big one on top
            return {box(x0, ym, x1, y1), box(x0, y0, xm, ym), box(xm, y0, x1, ym)};
        if (how == "Below")
            return {box(x0, y0, x1, ym), box(x0, ym, xm, y1), box(xm, ym, x1, y1)};
        if (how == "Left")
            return {box(x0, y0, xm, y1), box(xm, y0, x1, ym), box(xm, ym, x1, y1)};
        return {box(xm, y0, x1, y1), box(x0, y0, xm, ym), box(x0, ym, xm, y1)};  // Right
    }
    static constexpr double kFitMarginMm = 10;  // the default plot margin
    static std::string formatScale(double n) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.4g", n);
        return buf;
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
        if (preselected) {
            // The Viewport panel and a selected viewport: the command flips the lock.
            if (auto lay = layout::findLayout(ctx.doc(), target))
                if (const layout::Viewport* v = lay->findViewport(vpId))
                    on = !v->locked;
        } else {
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
        ko.keywords = {"Freeze", "Thaw", "NewFreeze", "NewThaw"};
        ko.defaultKeyword = "Freeze";
        auto action = co_await ctx.getKeyword(
            "Viewport layer [Freeze/Thaw/NewFreeze/NewThaw] (New: for viewports made later)", ko);
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
        if (action.keyword == "NewFreeze" || action.keyword == "NewThaw") {
            const bool freeze = action.keyword == "NewFreeze";
            report(ctx,
                   layout::setLayerFrozenInNewViewports(ctx.doc(), ctx.tx(), layer->id, freeze),
                   layer->name + (freeze ? " will be frozen" : " will not be frozen") +
                       " in new viewports.");
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
        if (mode.keyword == "Object")
            co_await pickOutlineObject(ctx, outline);
        else
            co_await pickOutline(ctx, outline);
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

// ALIGNSPACE: line a viewport's model view up with points on the sheet. One pair of points (a model
// point and where it must appear on the sheet) pans the view; a second pair also sets the scale.
class AlignSpaceCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ALIGNSPACE"; }
    std::string_view description() const override {
        return "Align the model view of a viewport to points on the sheet";
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
        const auto lay = layout::findLayout(ctx.doc(), target);
        const layout::Viewport* vp = lay ? lay->findViewport(vpId) : nullptr;
        if (!vp)
            co_return;
        const Vec2 pc{(vp->paper.min.x + vp->paper.max.x) / 2,
                      (vp->paper.min.y + vp->paper.max.y) / 2};
        auto m1 = co_await ctx.getPoint("Specify first model point");
        if (!m1.ok())
            co_return;
        cmd::PointOptions so;
        so.base = m1.point;
        auto s1 = co_await ctx.getPoint("Specify where it goes on the sheet (mm)", so);
        if (!s1.ok())
            co_return;
        cmd::PointOptions o2;
        o2.allowNone = true;
        auto m2 = co_await ctx.getPoint("Specify second model point, or Enter to only move", o2);
        double n = vp->scaleN;
        if (m2.ok()) {
            auto s2 =
                co_await ctx.getPoint("Specify where the second point goes on the sheet (mm)");
            if (!s2.ok())
                co_return;
            const double dm = std::hypot(m2.point.x - m1.point.x, m2.point.y - m1.point.y);
            const double ds = std::hypot(s2.point.x - s1.point.x, s2.point.y - s1.point.y);
            if (!(dm > 0) || !(ds > 0)) {
                ctx.message("The two points must be apart.");
                co_return;
            }
            n = dm / ds;
            auto r = layout::setViewportScale(ctx.tx(), target, vpId, n);
            if (!r) {
                ctx.message(r.error().message);
                co_return;
            }
        } else if (m2.status == Status::Cancel) {
            co_return;
        }
        const Vec2 centre{m1.point.x - (s1.point.x - pc.x) * n,
                          m1.point.y - (s1.point.y - pc.y) * n};
        report(ctx, layout::setViewportCenter(ctx.tx(), target, vpId, centre),
               "Viewport " + std::to_string(vpId) + " aligned.");
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

// MVSETUP: one-step sheet setup. Makes a layout, one viewport inside the margins at a scale (or
// fitted to the drawing) and, if asked, a title block.
// MVSETUP asks for a viewport scale straight away. LAYOUTWIZARD (the wizard flag) first asks
// whether the sheet gets a viewport at all, so an empty sheet can be made too.
class MvsetupCommand final : public cmd::Command {
public:
    explicit MvsetupCommand(bool wizard = false) : wizard_(wizard) {}
    std::string_view name() const override { return wizard_ ? "LAYOUTWIZARD" : "MVSETUP"; }
    std::string_view description() const override {
        return wizard_ ? "Make a new layout step by step: paper size, title block, viewport"
                       : "Set up a new sheet: paper size, one viewport at a scale, optional title "
                         "block";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        cmd::StringOptions no;
        const std::string defName =
            "Sheet " + std::to_string(layout::layouts(ctx.doc()).size() + 1);
        no.defaultValue = defName;
        auto name = co_await ctx.getString("Layout name <" + defName + ">", no);
        if (!name.ok())
            co_return;
        cmd::StringOptions po;
        po.defaultValue = "A3";
        auto paper = co_await ctx.getString("Paper size (A0-A4, ANSI A-E) <A3>", po);
        if (!paper.ok())
            co_return;
        std::string paperName = paper.text;
        for (const auto& p : exportlib::paperSizes())
            if (lower(p.name) == lower(paper.text))
                paperName = p.name;
        cmd::KeywordOptions ko;
        ko.keywords = {"Landscape", "Portrait"};
        ko.defaultKeyword = "Landscape";
        auto orient = co_await ctx.getKeyword("Orientation [Landscape/Portrait] <Landscape>", ko);
        if (!orient.ok())
            co_return;
        bool withViewport = true;
        if (wizard_) {
            cmd::KeywordOptions vo;
            vo.keywords = {"Yes", "No"};
            vo.defaultKeyword = "Yes";
            auto vp = co_await ctx.getKeyword("Add a viewport [Yes/No] <Yes>", vo);
            if (!vp.ok())
                co_return;
            withViewport = vp.keyword == "Yes";
        }
        cmd::StringOptions so;
        so.defaultValue = "Fit";
        cmd::StringResult scale{};
        if (withViewport) {
            scale = co_await ctx.getString("Viewport scale (1:N) or [Fit] <Fit>", so);
            if (!scale.ok())
                co_return;
        }
        const bool fit = !withViewport || lower(scale.text) == "fit" || lower(scale.text) == "f";
        double n = 0;
        if (withViewport && !fit && !parseScaleN(scale.text, n)) {
            ctx.message("Not a scale: \"" + scale.text + "\". Use 1:N.");
            co_return;
        }
        cmd::KeywordOptions to;
        to.keywords = {"Yes", "No"};
        to.defaultKeyword = "No";
        auto tb = co_await ctx.getKeyword("Add a title block [Yes/No] <No>", to);
        if (!tb.ok())
            co_return;
        layout::TitleBlockFields f;
        if (tb.keyword == "Yes") {
            cmd::StringOptions eo;
            eo.allowEmpty = true;
            auto project = co_await ctx.getString("Project", eo);
            if (!project.ok())
                co_return;
            auto drawing = co_await ctx.getString("Drawing", eo);
            if (!drawing.ok())
                co_return;
            f.project = project.text;
            f.drawing = drawing.text;
            f.scale = fit ? std::string() : scale.text;
        }

        const bool landscape = orient.keyword == "Landscape";
        auto made = layout::createLayout(ctx.tx(), name.text, paperName, landscape);
        if (!made) {
            ctx.message(made.error().message);
            co_return;
        }
        const auto lay = layout::findLayout(ctx.doc(), name.text);
        if (!lay)
            co_return;
        const double m = std::min({10.0, lay->widthMm / 4, lay->heightMm / 4});
        BBox2 rect;
        rect.expand({m, m});
        rect.expand({lay->widthMm - m, lay->heightMm - m});
        const BBox2 box = modelExtents(ctx.doc());
        Vec2 centre{};
        if (!box.empty())
            centre = {(box.min.x + box.max.x) / 2, (box.min.y + box.max.y) / 2};
        if (fit) {
            n = box.empty() ? 1
                            : std::max(box.width() / rect.width(), box.height() / rect.height());
            if (!(n > 0))
                n = 1;
        }
        if (withViewport)
            if (auto id = layout::addViewport(ctx.tx(), name.text, rect, centre, n); !id) {
                ctx.message(id.error().message);
                co_return;
            }
        if (tb.keyword == "Yes")
            if (auto r = layout::addTitleBlock(ctx.tx(), name.text, f); !r)
                ctx.message(r.error().message);
        ctx.host().setSetting("layout.current", name.text);
        ctx.host().setSetting("layout.show", name.text);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.4g", n);
        if (withViewport)
            ctx.message("Sheet \"" + name.text + "\" set up with a viewport at 1:" + buf + ".");
        else
            ctx.message("Sheet \"" + name.text + "\" set up without a viewport.");
    }

private:
    bool wizard_;
};

// The layout as a new drawing: see ExportLayoutCommand.
Result<std::unique_ptr<Document>> layoutDrawing(const Document& doc, const std::string& name) {
    const auto found = layout::findLayout(doc, name);
    if (!found)
        return Error{ErrorCode::NotFound, "No layout named \"" + name + "\""};
    const layout::Layout& l = *found;
    auto out = std::make_unique<Document>();
    Transaction tx = out->begin("EXPORTLAYOUT");
    const std::vector<EntityId> model = doc.modelSpace();
    for (const layout::Viewport& vp : l.viewports) {
        if (!vp.on || vp.paper.empty() || !(vp.scaleN > 0))
            continue;
        // The model window the viewport shows (the same one the plotter uses).
        const Vec2 half{vp.paper.width() * vp.scaleN / 2, vp.paper.height() * vp.scaleN / 2};
        BBox2 window;
        window.expand(vp.center - half);
        window.expand(vp.center + half);
        const BBox2 reach = window.inflated(vp.scaleN);
        std::vector<EntityId> ids;
        for (EntityId id : model) {
            const Entity* e = doc.find(id);
            if (!e || !e->common.visible || !doc.effective(e->common).visible())
                continue;
            if (layout::frozenInViewport(doc, vp, e->common.layer) ||
                (vp.level.value != 0 && e->common.level != vp.level))
                continue;
            if (e->common.annotative && !e->common.annoScales.empty() &&
                std::find(e->common.annoScales.begin(), e->common.annoScales.end(), vp.scaleN) ==
                    e->common.annoScales.end())
                continue;  // made for other scales
            const BBox2 box = geom::bounds(*e, doc);
            if (!box.empty() && !box.intersects(reach))
                continue;
            ids.push_back(id);
        }
        clipboard::Clip clip = clipboard::capture(doc, ids, Vec2{0, 0});
        if (clip.empty())
            continue;
        const Vec2 mid{(vp.paper.min.x + vp.paper.max.x) / 2,
                       (vp.paper.min.y + vp.paper.max.y) / 2};
        const Transform2 t = Transform2::translate(mid) *
                             Transform2::scale(1 / vp.scaleN, 1 / vp.scaleN) *
                             Transform2::translate({-vp.center.x, -vp.center.y});
        std::vector<Entity> moved;
        std::vector<std::string> styles;
        for (std::size_t i = 0; i < clip.entities.size(); ++i)
            if (auto m = geom::transformed(clip.entities[i], t)) {
                moved.push_back(std::move(m.value()));
                styles.push_back(i < clip.plotStyles.size() ? clip.plotStyles[i] : std::string());
            }
        clip.entities = std::move(moved);
        clip.plotStyles = std::move(styles);
        if (clip.empty())
            continue;
        clip.base = {0, 0};
        if (auto r = clipboard::paste(tx, clip, clipboard::PasteOptions{}); !r)
            return r.error();
    }
    clipboard::Clip sheet = clipboard::captureBlock(doc, l.content);
    if (!sheet.empty()) {
        sheet.base = {0, 0};
        if (auto r = clipboard::paste(tx, sheet, clipboard::PasteOptions{}); !r)
            return r.error();
    }
    tx.commit();
    return out;
}

// EXPORTLAYOUT: the active layout as a new model-space drawing at paper size (millimetres). Each
// viewport's visible model objects are copied through the viewport's scale and position; objects
// that only partly reach a viewport are kept whole. The sheet's own objects come last, on top.
// Written to .tbcad, or DXF when the name ends in .dxf.
class ExportLayoutCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "EXPORTLAYOUT"; }
    std::string_view description() const override {
        return "Write the active layout as a new model-space drawing";
    }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::string layoutName;
        if (!requireLayout(ctx, layoutName))
            co_return;
        const std::string def = layoutName + ".tbcad";
        auto f = co_await ctx.getString("Enter the file name <" + def + ">",
                                        cmd::StringOptions{.defaultValue = def});
        if (!f.ok())
            co_return;
        std::string file = cmd::trim(f.text);
        std::filesystem::path path(std::u8string(file.begin(), file.end()));
        if (path.extension().empty()) {
            file += ".tbcad";
            path = std::filesystem::path(std::u8string(file.begin(), file.end()));
        }
        const bool asDxf = lower(path.extension().string()) == ".dxf";

        auto out = layoutDrawing(ctx.doc(), layoutName);
        if (!out) {
            ctx.message(out.error().message);
            co_return;
        }
        if (out.value()->modelSpaceCount() == 0) {
            ctx.message("Layout \"" + layoutName + "\" has nothing to write.");
            co_return;
        }
        if (asDxf) {
            dxf::Report report;
            auto res = dxf::exportDxf(*out.value(), dxf::ExportOptions{}, report);
            if (!res) {
                ctx.message("Cannot write " + file + ": " + res.error().message);
                co_return;
            }
            const auto& bytes = res.value().bytes;
            auto saved = io::saveTbcadBytes(
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                              bytes.size()),
                path, io::WriteOptions{.keepBackup = false});
            if (!saved) {
                ctx.message("Cannot write " + file + ": " + saved.error().message);
                co_return;
            }
        } else if (auto r =
                       io::writeTbcad(*out.value(), path, io::WriteOptions{.keepBackup = false});
                   !r) {
            ctx.message("Cannot write " + file + ": " + r.error().message);
            co_return;
        }
        ctx.message("Layout \"" + layoutName + "\" written to " + file + " (" +
                    std::to_string(out.value()->modelSpaceCount()) + " objects).");
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
    (void)registry.add(std::make_unique<AlignSpaceCommand>());
    (void)registry.add(std::make_unique<TitleBlockCommand>());
    (void)registry.add(std::make_unique<MvsetupCommand>());
    (void)registry.add(std::make_unique<MvsetupCommand>(true));
    (void)registry.add(std::make_unique<ExportLayoutCommand>());
}

}  // namespace tbcad::layoutcmd
