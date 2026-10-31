// SHEETLAYER: how layers look on one layout sheet, apart from the drawing and from other sheets
// (show or hide, colour, linetype, lineweight, transparency, plot). The "This sheet" mode of the
// Layers panel runs it. See layout::LayerOverride.
#include <algorithm>
#include <cctype>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/layout/layout.h"

namespace tbcad::layers {

namespace {

using cmd::CommandContext;
using cmd::Task;

std::string lowerText(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

// The sheet in front of the user: the current layout, else the first one.
std::string currentSheet(const cmd::IHost& host, const Document& doc) {
    if (auto cur = host.setting("layout.current"); cur && !cur->empty())
        if (auto l = layout::findLayout(doc, *cur))
            return l->name;
    auto all = layout::layouts(doc);
    return all.empty() ? std::string() : all.front().name;
}

bool resetWord(const std::string& v) {
    const std::string l = lowerText(v);
    return l == "drawing" || l == "reset" || l == "bylayer";
}

std::string describe(const Document& doc, const layout::LayerOverride& o) {
    std::string t;
    auto add = [&](const std::string& part) { t += (t.empty() ? "" : ", ") + part; };
    if (o.on)
        add(*o.on ? "shown" : "hidden");
    if (o.color)
        add("colour " + colorName(*o.color));
    if (o.linetype)
        add("linetype " + linetypeName(doc, *o.linetype));
    if (o.lineweight)
        add("lineweight " + lineweightName(*o.lineweight));
    if (o.transparency)
        add("transparency " + std::to_string(*o.transparency) + "%");
    if (o.plot)
        add(*o.plot ? "plots" : "does not plot");
    return t;
}

class SheetLayerCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SHEETLAYER"; }
    std::vector<std::string> defaultAliases() const override { return {"SHL"}; }
    std::string_view description() const override {
        return "Show, hide or restyle layers on the current layout sheet only";
    }

    Task run(CommandContext& ctx) override {
        ctx.commitOnCancel();
        ctx.setUndoLabel("SHEETLAYER");
        std::string sheet = currentSheet(ctx.host(), ctx.doc());
        if (sheet.empty()) {
            ctx.message("This drawing has no layouts. Use LAYOUT > New.");
            co_return;
        }
        cmd::KeywordOptions ko;
        ko.keywords = {"On",   "Off",    "Color", "Linetype", "Lineweight", "Transparency",
                       "Plot", "NoPlot", "Reset", "ResetAll", "List",       "Sheet"};
        ko.aliases = {{"LT", "Linetype"}, {"LW", "Lineweight"}, {"O", "On"}, {"P", "Plot"}};
        // One option after another, as -LAYER; Enter ends.
        while (true) {
            auto option = co_await ctx.getKeyword(
                "Layers on sheet \"" + sheet +
                    "\" [On/Off/Color/Linetype/Lineweight/Transparency/Plot/NoPlot/Reset/"
                    "ResetAll/List/Sheet]",
                ko);
            if (!option.ok())
                co_return;
            if (option.keyword == "Sheet") {
                auto name = co_await ctx.getString("Layout name");
                if (!name.ok() || name.text.empty())
                    co_return;
                auto l = layout::findLayout(ctx.doc(), name.text);
                if (!l) {
                    ctx.message("No layout named \"" + name.text + "\".");
                    co_return;
                }
                sheet = l->name;
                continue;
            }
            bool ok = true;
            co_await runOption(ctx, sheet, option.keyword, ok);
            if (!ok)
                co_return;
        }
    }

private:
    static Task runOption(CommandContext& ctx, std::string sheet, std::string k, bool& ok) {
        const Document& doc = ctx.doc();
        if (k == "List") {
            auto l = layout::findLayout(doc, sheet);
            if (!l || l->layerOverrides.empty()) {
                ctx.message("Sheet \"" + sheet + "\" shows every layer as the drawing does.");
                co_return;
            }
            ctx.message("Sheet \"" + sheet + "\":");
            for (const layout::LayerOverride& o : l->layerOverrides)
                if (doc.layer(o.layer))
                    ctx.message("  " + layerPath(doc, o.layer) + ": " + describe(doc, o));
            co_return;
        }
        if (k == "ResetAll") {
            if (auto r = layout::clearSheetLayerOverrides(ctx.tx(), sheet); !r)
                ctx.message(r.error().message);
            else
                ctx.message("Sheet \"" + sheet + "\" shows the layers as the drawing does.");
            co_return;
        }
        ok = false;
        std::string value;
        const bool needsValue =
            k == "Color" || k == "Linetype" || k == "Lineweight" || k == "Transparency";
        if (needsValue) {
            auto v = co_await ctx.getString(
                k == "Color"      ? "Colour on this sheet (name, 1-255, r,g,b or Drawing)"
                : k == "Linetype" ? "Linetype on this sheet (or Drawing)"
                : k == "Lineweight"
                    ? "Lineweight in mm on this sheet (Default or Drawing)"
                    : "Transparency in percent on this sheet (0 to 90, or Drawing)");
            if (!v.ok() || v.text.empty())
                co_return;
            value = v.text;
        }
        auto refs = co_await ctx.getString("Layer name(s), * for all");
        if (!refs.ok() || refs.text.empty())
            co_return;
        auto ids = resolveLayers(doc, refs.text);
        if (!ids) {
            ctx.message(ids.error().message);
            co_return;
        }
        const bool reset = needsValue && resetWord(value);
        std::optional<Color> color;
        std::optional<LinetypeId> linetype;
        std::optional<LineWeight> weight;
        std::optional<int> transparency;
        if (needsValue && !reset) {
            if (k == "Color") {
                auto c = parseColor(value);
                if (!c ||
                    (c.value().kind != ColorKind::Index && c.value().kind != ColorKind::Rgb)) {
                    ctx.message(c ? "A sheet colour is a palette number, a name or r,g,b."
                                  : c.error().message);
                    co_return;
                }
                color = c.value();
            } else if (k == "Linetype") {
                auto lt = resolveLinetype(doc, ctx.tx(), value);
                if (!lt || !ctx.doc().linetype(lt.value())) {
                    ctx.message(lt ? "A sheet linetype is a loaded linetype." : lt.error().message);
                    co_return;
                }
                linetype = lt.value();
            } else if (k == "Lineweight") {
                auto lw = parseLineweight(value);
                if (!lw || !(lw.value().isValue() || lw.value().isDefault())) {
                    ctx.message(lw ? "A sheet lineweight is Default or a weight in mm."
                                   : lw.error().message);
                    co_return;
                }
                weight = lw.value();
            } else {
                auto tr = parseTransparency(value);
                if (!tr || tr.value() < 0 || tr.value() > 90) {
                    ctx.message("A sheet transparency is 0 to 90.");
                    co_return;
                }
                transparency = tr.value();
            }
        }
        for (LayerId id : ids.value()) {
            if (k == "Reset") {
                if (auto r = layout::clearSheetLayerOverrides(ctx.tx(), sheet, id); !r) {
                    ctx.message(r.error().message);
                    co_return;
                }
                continue;
            }
            const auto lay = layout::findLayout(ctx.doc(), sheet);
            if (!lay)
                co_return;
            layout::LayerOverride o;
            if (const layout::LayerOverride* old = lay->findOverride(id))
                o = *old;
            o.layer = id;
            if (k == "On" || k == "Off")
                o.on = k == "On";
            else if (k == "Plot" || k == "NoPlot")
                o.plot = k == "Plot";
            else if (k == "Color")
                o.color = color;
            else if (k == "Linetype")
                o.linetype = linetype;
            else if (k == "Lineweight")
                o.lineweight = weight;
            else if (k == "Transparency")
                o.transparency = transparency;
            // Shown or plotting as the drawing has it: nothing to keep for this sheet.
            const EffectiveLayer e = ctx.doc().effective(id);
            if (o.on && *o.on == e.visible())
                o.on.reset();
            if (o.plot && *o.plot == e.plot)
                o.plot.reset();
            if (auto r = layout::setSheetLayerOverride(ctx.tx(), sheet, o); !r) {
                ctx.message(r.error().message);
                co_return;
            }
        }
        ok = true;
        const std::size_t n = ids.value().size();
        const std::string what =
            n == 1 ? layerPath(ctx.doc(), ids.value().front()) : std::to_string(n) + " layers";
        if (k == "Reset" || reset)
            ctx.message(what + " looks as in the drawing on sheet \"" + sheet + "\".");
        else
            ctx.message(what + " changed on sheet \"" + sheet + "\" only.");
    }
};

}  // namespace

std::unique_ptr<cmd::Command> makeSheetLayerCommand() {
    return std::make_unique<SheetLayerCommand>();
}

}  // namespace tbcad::layers
