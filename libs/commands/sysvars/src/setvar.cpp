// SETVAR: read and change a drawing variable by its name. The variables are the ones the other
// commands (LTSCALE, COLOR, ...) already set; the names follow the usual ones.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

#include "commands.h"
#include "tbcad/core/groups.h"
#include "tbcad/layers/layer_ops.h"

namespace tbcad::sysvars {

namespace {

using cmd::CommandContext;
using cmd::Task;

const char* unitText(LengthUnit u) {
    switch (u) {
        case LengthUnit::Millimeter:
            return "Millimeters";
        case LengthUnit::Centimeter:
            return "Centimeters";
        case LengthUnit::Meter:
            return "Meters";
        case LengthUnit::Inch:
            return "Inches";
        case LengthUnit::Foot:
            return "Feet";
    }
    return "Millimeters";
}

std::string upper(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// 0/1 and On/Off.
bool parseSwitch(std::string_view v, bool& on) {
    const std::string s = upper(std::string(v));
    if (s == "0" || s == "OFF") {
        on = false;
        return true;
    }
    if (s == "1" || s == "ON") {
        on = true;
        return true;
    }
    return false;
}

struct Var {
    const char* name;
    const char* help;
    std::function<std::string(const Document&)> get;
    // Empty string = done, otherwise the error message.
    std::function<std::string(const Document&, std::string_view, Header&)> set;
};

const std::vector<Var>& variables() {
    static const std::vector<Var> vars = {
        {"LTSCALE", "linetype scale factor",
         [](const Document& d) { return formatDouble(d.header().ltScale); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             char* end = nullptr;
             const std::string s(v);
             const double x = std::strtod(s.c_str(), &end);
             if (s.empty() || *end != 0 || !(x >= 1e-6 && x <= 1e6))
                 return "LTSCALE needs a number between 1e-6 and 1e6.";
             h.ltScale = x;
             return {};
         }},
        {"CELTSCALE", "linetype scale of new objects",
         [](const Document& d) { return formatDouble(ceLtScale(d.header())); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             char* end = nullptr;
             const std::string s(v);
             const double x = std::strtod(s.c_str(), &end);
             if (s.empty() || *end != 0 || !(x >= 1e-6 && x <= 1e6))
                 return "CELTSCALE needs a number between 1e-6 and 1e6.";
             h.custom[kCeLtScaleKey] = formatDouble(x);
             return {};
         }},
        {"PSLTSCALE", "linetypes in layout viewports follow the viewport scale (1) or not (0)",
         [](const Document& d) { return std::string(psLtScale(d.header()) ? "1" : "0"); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "PSLTSCALE is 0 or 1.";
             h.custom[kPsLtScaleKey] = on ? "1" : "0";
             return {};
         }},
        {"MSLTSCALE", "linetypes in model space follow the annotation scale (1) or not (0)",
         [](const Document& d) { return std::string(msLtScale(d.header()) ? "1" : "0"); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "MSLTSCALE is 0 or 1.";
             h.custom[kMsLtScaleKey] = on ? "1" : "0";
             return {};
         }},
        {"PLINEGEN", "linetype runs on across polyline vertices (1) or restarts at each (0)",
         [](const Document& d) { return std::string(plineGen(d.header()) ? "1" : "0"); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "PLINEGEN is 0 or 1.";
             h.custom[kPlineGenKey] = on ? "1" : "0";
             return {};
         }},
        {"ANNOAUTOSCALE",
         "changing the annotation scale adds it to annotative objects (1) or not (0)",
         [](const Document& d) { return std::string(annoAutoScale(d.header()) ? "1" : "0"); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "ANNOAUTOSCALE is 0 or 1.";
             h.custom[kAnnoAutoScaleKey] = on ? "1" : "0";
             return {};
         }},
        {"ANNOALLVISIBLE", "annotative objects made for other scales are shown (1) or hidden (0)",
         [](const Document& d) { return std::string(annoAllVisible(d.header()) ? "1" : "0"); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "ANNOALLVISIBLE is 0 or 1.";
             h.custom[kAnnoAllVisibleKey] = on ? "1" : "0";
             return {};
         }},
        {"SELECTIONANNODISPLAY",
         "a selected annotative object also shows its other scales, dimmed (1) or not (0)",
         [](const Document& d) {
             return std::string(selectionAnnoDisplay(d.header()) ? "1" : "0");
         },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "SELECTIONANNODISPLAY is 0 or 1.";
             h.custom[kSelAnnoDisplayKey] = on ? "1" : "0";
             return {};
         }},
        {"QTEXTMODE", "text shows as boxes (1) or as text (0)",
         [](const Document& d) { return std::string(quickText(d.header()) ? "1" : "0"); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "QTEXTMODE is 0 or 1.";
             h.custom[kQTextKey] = on ? "1" : "0";
             return {};
         }},
        {"FILLMODE", "solid fills show (1) or only their outlines do (0)",
         [](const Document& d) { return std::string(fillMode(d.header()) ? "1" : "0"); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "FILLMODE is 0 or 1.";
             h.custom[kFillModeKey] = on ? "1" : "0";
             return {};
         }},
        {"ATTMODE", "attributes: 0 hidden, 1 as defined, 2 all shown",
         [](const Document& d) {
             const int m = attDisplay(d.header());
             return std::string(m == 0 ? "1" : m == 1 ? "2" : "0");
         },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             if (v == "0")
                 h.custom[kAttDispKey] = "2";
             else if (v == "1")
                 h.custom[kAttDispKey] = "0";
             else if (v == "2")
                 h.custom[kAttDispKey] = "1";
             else
                 return "ATTMODE is 0, 1 or 2.";
             return {};
         }},
        {"MIRRTEXT", "MIRROR reflects text too (1) or keeps it readable (0)",
         [](const Document& d) { return std::string(mirrText(d.header()) ? "1" : "0"); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             bool on = false;
             if (!parseSwitch(v, on))
                 return "MIRRTEXT is 0 or 1.";
             h.custom[kMirrTextKey] = on ? "1" : "0";
             return {};
         }},
        {"PICKSTYLE", "picks select whole groups (1), associative hatch boundaries (2), both (3)",
         [](const Document& d) { return std::to_string(pickStyle(d.header())); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             if (v.size() != 1 || v[0] < '0' || v[0] > '3')
                 return "PICKSTYLE is 0, 1, 2 or 3.";
             h.custom[kPickStyleKey] = std::string(v);
             return {};
         }},
        {"CECOLOR", "colour of new objects",
         [](const Document& d) { return layers::colorName(d.header().currentColor); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             auto c = layers::parseColor(v);
             if (!c)
                 return c.error().message;
             if (c.value().kind == ColorKind::ByParent)
                 return "ByParent only applies to sub-layers.";
             h.currentColor = c.value();
             return {};
         }},
        {"CELWEIGHT", "lineweight of new objects",
         [](const Document& d) { return layers::lineweightName(d.header().currentLineweight); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             auto w = layers::parseLineweight(v);
             if (!w)
                 return w.error().message;
             if (w.value().isByParent())
                 return "ByParent only applies to sub-layers.";
             h.currentLineweight = w.value();
             return {};
         }},
        {"CELTYPE", "linetype of new objects",
         [](const Document& d) { return layers::linetypeName(d, d.header().currentLinetype); },
         [](const Document& d, std::string_view v, Header& h) -> std::string {
             auto t = layers::parseLinetype(d, v);
             if (!t)
                 return t.error().message;
             h.currentLinetype = t.value();
             return {};
         }},
        {"CETRANSPARENCY", "transparency of new objects",
         [](const Document& d) { return layers::transparencyName(d.header().currentTransparency); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             auto t = layers::parseTransparency(v);
             if (!t)
                 return t.error().message;
             h.currentTransparency = static_cast<std::int8_t>(t.value());
             return {};
         }},
        {"CLAYER", "current layer",
         [](const Document& d) {
             const Layer* l = d.layer(d.header().currentLayer);
             return l ? l->name : std::string("0");
         },
         [](const Document& d, std::string_view v, Header& h) -> std::string {
             const Layer* l = d.layerByName(v);
             if (!l)
                 return "There is no layer named " + std::string(v) + ".";
             h.currentLayer = l->id;
             return {};
         }},
        {"INSUNITS", "length unit of the drawing (objects are not scaled)",
         [](const Document& d) { return std::string(unitText(d.header().units)); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             const std::string u = upper(std::string(v));
             for (LengthUnit c : {LengthUnit::Millimeter, LengthUnit::Centimeter, LengthUnit::Meter,
                                  LengthUnit::Inch, LengthUnit::Foot})
                 if (upper(unitText(c)) == u) {
                     h.units = c;
                     return {};
                 }
             return "INSUNITS is one of Millimeters, Centimeters, Meters, Inches, Feet.";
         }},
    };
    return vars;
}

class SetvarCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SETVAR"; }
    std::vector<std::string> defaultAliases() const override { return {"SET"}; }
    std::string_view description() const override {
        return "Show or change a drawing variable by name (? lists them)";
    }
    Task run(CommandContext& ctx) override {
        auto in = co_await ctx.getString("Variable name or [?]", cmd::StringOptions{});
        if (!in.ok())
            co_return;
        const std::string name = upper(std::string(cmd::trim(in.text)));
        if (name == "?" || name.empty()) {
            for (const Var& v : variables())
                ctx.message(std::string(v.name) + " = " + v.get(ctx.doc()) + "  (" + v.help + ")");
            co_return;
        }
        const auto it = std::find_if(variables().begin(), variables().end(),
                                     [&](const Var& v) { return name == v.name; });
        if (it == variables().end()) {
            ctx.message("Unknown variable " + name + ". Type SETVAR ? to list them.");
            co_return;
        }
        cmd::StringOptions so;
        so.defaultValue = it->get(ctx.doc());
        auto val =
            co_await ctx.getString("New value for " + name + " <" + *so.defaultValue + ">", so);
        if (!val.ok())
            co_return;
        const std::string text(cmd::trim(val.text));
        ctx.setUndoLabel(name);
        std::string err;
        auto r = ctx.tx().modifyHeader([&](Header& h) { err = it->set(ctx.doc(), text, h); });
        if (!r)
            ctx.message(r.error().message);
        else if (!err.empty())
            ctx.message(err);
        else
            ctx.message(name + " = " + it->get(ctx.doc()));
    }
};

}  // namespace

namespace {
// A command named like one variable (CELTSCALE, PLINEGEN, ...): asks for the new value.
class VarCommand final : public cmd::Command {
public:
    VarCommand(std::string name, std::string description)
        : name_(std::move(name)), description_(std::move(description)) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return description_; }
    Task run(CommandContext& ctx) override {
        const auto it = std::find_if(variables().begin(), variables().end(),
                                     [&](const Var& v) { return name_ == v.name; });
        if (it == variables().end())
            co_return;
        cmd::StringOptions so;
        so.defaultValue = it->get(ctx.doc());
        auto val = co_await ctx.getString(
            "Enter new value for " + name_ + " <" + *so.defaultValue + ">", so);
        if (!val.ok())
            co_return;
        const std::string text(cmd::trim(val.text));
        ctx.setUndoLabel(name_);
        std::string err;
        auto r = ctx.tx().modifyHeader([&](Header& h) { err = it->set(ctx.doc(), text, h); });
        if (!r)
            ctx.message(r.error().message);
        else if (!err.empty())
            ctx.message(err);
        else
            ctx.message(name_ + " = " + it->get(ctx.doc()));
    }

private:
    std::string name_;
    std::string description_;
};
}  // namespace

std::unique_ptr<cmd::Command> makeVarCommand(const std::string& name) {
    const auto it = std::find_if(variables().begin(), variables().end(),
                                 [&](const Var& v) { return name == v.name; });
    return std::make_unique<VarCommand>(name, it == variables().end() ? "" : it->help);
}

namespace {
// LWDISPLAY: show lineweights on screen (the LWT button of the status bar). A setting of this
// installation, not of the drawing.
class LwDisplayCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LWDISPLAY"; }
    std::string_view description() const override {
        return "Show lineweights on screen (On/Off, same as the LWT button)";
    }
    Task run(CommandContext& ctx) override {
        const auto cur = ctx.host().setting("view.lineweights");
        const bool on = !cur || *cur == "1" || *cur == "true";
        cmd::StringOptions so;
        so.defaultValue = on ? "1" : "0";
        auto val = co_await ctx.getString(
            std::string("Enter new value for LWDISPLAY <") + *so.defaultValue + ">", so);
        if (!val.ok())
            co_return;
        bool want = false;
        if (!parseSwitch(cmd::trim(val.text), want)) {
            ctx.message("LWDISPLAY is 0 or 1.");
            co_return;
        }
        ctx.host().setSetting("view.lineweights", want ? "1" : "0");
        ctx.message(std::string("LWDISPLAY = ") + (want ? "1" : "0"));
    }
};
}  // namespace

std::unique_ptr<cmd::Command> makeLwDisplayCommand() {
    return std::make_unique<LwDisplayCommand>();
}

std::unique_ptr<cmd::Command> makeSetvarCommand() {
    return std::make_unique<SetvarCommand>();
}

}  // namespace tbcad::sysvars
