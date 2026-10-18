// SETVAR: read and change a drawing variable by its name. The variables are the ones the other
// commands (LTSCALE, COLOR, ...) already set; the names follow the usual ones.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

#include "commands.h"
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

std::unique_ptr<cmd::Command> makeSetvarCommand() {
    return std::make_unique<SetvarCommand>();
}

}  // namespace tbcad::sysvars
