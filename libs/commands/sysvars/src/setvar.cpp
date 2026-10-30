// SETVAR: read and change a drawing variable by its name. The variables are the ones the other
// commands (LTSCALE, COLOR, ...) already set; the names follow the usual ones.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "commands.h"
#include "tbcad/core/groups.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/layers/layer_ops.h"
#include "tbcad/sysvars/variables.h"

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

// A whole number and nothing else.
bool parseInt(std::string_view v, int& out) {
    const std::string t(v);
    char* end = nullptr;
    const long n = std::strtol(t.c_str(), &end, 10);
    if (t.empty() || end != t.c_str() + t.size() || n < -1000000 || n > 1000000)
        return false;
    out = static_cast<int>(n);
    return true;
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
        {"LWDEFAULT", "lineweight that \"Default\" draws and plots as, in hundredths of a mm",
         [](const Document& d) {
             return std::to_string(
                 static_cast<int>(std::lround(defaultLineweightMm(d.header()) * 100)));
         },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             char* end = nullptr;
             const std::string text(v);
             const double n = std::strtod(text.c_str(), &end);
             if (end == text.c_str() || *end || n < 0 || n > 211)
                 return "LWDEFAULT is 0 to 211 (hundredths of a millimetre, 25 = 0.25 mm).";
             h.custom[kLwDefaultKey] = std::to_string(static_cast<int>(std::lround(n)));
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
        {"HPORIGIN", "pattern origin of new hatches, x,y",
         [](const Document& d) {
             const Vec2 o = hatchOrigin(d.header());
             char buf[80];
             std::snprintf(buf, sizeof buf, "%.10g,%.10g", o.x, o.y);
             return std::string(buf);
         },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             const std::string t(v);
             auto comma = t.find(',');
             if (comma == std::string::npos)
                 comma = t.find(' ');
             char* end = nullptr;
             const double x = comma == std::string::npos ? 0 : std::strtod(t.c_str(), &end);
             const bool xOk = comma != std::string::npos && end == t.c_str() + comma;
             const double y = xOk ? std::strtod(t.c_str() + comma + 1, &end) : 0;
             if (!xOk || end != t.c_str() + t.size() || !std::isfinite(x) || !std::isfinite(y))
                 return "HPORIGIN is a point: x,y.";
             h.custom[kHpOriginKey] = t.substr(0, comma) + "," + t.substr(comma + 1);
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
        {"LUNITS",
         "how lengths are shown: 1 scientific, 2 decimal, 3 engineering, "
         "4 architectural, 5 fractional",
         [](const Document& d) { return std::to_string(lengthUnitsType(d.header())); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             int n = 0;
             if (!parseInt(v, n) || n < 1 || n > 5)
                 return "LUNITS is 1 to 5.";
             setLengthUnitsType(h, n);
             return {};
         }},
        {"LUPREC", "decimal places of lengths (0-8)",
         [](const Document& d) { return std::to_string(linearPrecision(d.header())); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             int n = 0;
             if (!parseInt(v, n) || n < 0 || n > 8)
                 return "LUPREC is 0 to 8.";
             setLinearPrecision(h, n);
             return {};
         }},
        {"AUNITS", "how angles are shown: 0 degrees, 1 deg/min/sec, 2 grads, 3 radians, 4 surveyor",
         [](const Document& d) { return std::to_string(angleUnitsType(d.header())); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             int n = 0;
             if (!parseInt(v, n) || n < 0 || n > 4)
                 return "AUNITS is 0 to 4.";
             setAngleUnitsType(h, n);
             return {};
         }},
        {"AUPREC", "decimal places of angles (0-8)",
         [](const Document& d) { return std::to_string(anglePrecision(d.header())); },
         [](const Document&, std::string_view v, Header& h) -> std::string {
             int n = 0;
             if (!parseInt(v, n) || n < 0 || n > 8)
                 return "AUPREC is 0 to 8.";
             setAnglePrecision(h, n);
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

// Variables kept as settings of this installation (drafting aids, text height), not in the
// drawing: typed as in AutoCAD scripts (OSMODE 35, ORTHOMODE 1, ...).
struct HostVar {
    const char* name;
    const char* help;
    std::function<std::string(const cmd::IHost&)> get;
    // Empty string = done, otherwise the error message.
    std::function<std::string(cmd::IHost&, std::string_view)> set;
};

// OSMODE bits, in AutoCAD's order; 16384 = object snap off.
constexpr std::pair<int, drafting::SnapMode> kOsmodeBits[] = {
    {1, drafting::SnapMode::Endpoint},
    {2, drafting::SnapMode::Midpoint},
    {4, drafting::SnapMode::Center},
    {8, drafting::SnapMode::Node},
    {16, drafting::SnapMode::Quadrant},
    {32, drafting::SnapMode::Intersection},
    {64, drafting::SnapMode::Insertion},
    {128, drafting::SnapMode::Perpendicular},
    {256, drafting::SnapMode::Tangent},
    {512, drafting::SnapMode::Nearest},
    {1024, drafting::SnapMode::GeometricCenter},
    {2048, drafting::SnapMode::ApparentIntersection},
    {4096, drafting::SnapMode::Extension},
    {8192, drafting::SnapMode::Parallel},
};
constexpr int kOsmodeOff = 16384;

// A 0/1 switch among the drafting aids (ORTHOMODE, SNAPMODE, GRIDMODE).
HostVar switchVar(const char* name, const char* help, const char* key,
                  bool drafting::DraftingSettings::*field) {
    return {name, help,
            [field](const cmd::IHost& host) {
                return std::string(drafting::DraftingSettings::read(host).*field ? "1" : "0");
            },
            [name, key](cmd::IHost& host, std::string_view v) -> std::string {
                bool on = false;
                if (!parseSwitch(v, on))
                    return std::string(name) + " is 0 or 1.";
                host.setSetting(key, on ? "1" : "0");
                // Ortho and polar tracking exclude each other.
                if (on && std::string_view(key) == drafting::keys::ortho)
                    host.setSetting(drafting::keys::polar, "0");
                return {};
            }};
}

const std::vector<HostVar>& hostVariables() {
    static const std::vector<HostVar> vars = {
        {"OSMODE",
         "object snap modes as AutoCAD's sum: 1 end, 2 mid, 4 cen, 8 node, 16 quad, 32 int, "
         "64 ins, 128 perp, 256 tan, 512 near, 1024 gcen, 2048 app, 4096 ext, 8192 par; +16384 "
         "= snap off",
         [](const cmd::IHost& host) {
             const auto s = drafting::DraftingSettings::read(host);
             int v = s.osnap ? 0 : kOsmodeOff;
             for (const auto& [bit, mode] : kOsmodeBits)
                 if (s.modes & drafting::maskOf(mode))
                     v += bit;
             return std::to_string(v);
         },
         [](cmd::IHost& host, std::string_view v) -> std::string {
             int n = 0;
             if (!parseInt(v, n) || n < 0 || n > 32767)
                 return "OSMODE is 0 to 32767.";
             drafting::SnapModeMask mask = 0;
             for (const auto& [bit, mode] : kOsmodeBits)
                 if (n & bit)
                     mask |= drafting::maskOf(mode);
             host.setSetting(drafting::keys::osnapModes, drafting::formatSnapModes(mask));
             host.setSetting(drafting::keys::osnap, (n & kOsmodeOff) || !mask ? "0" : "1");
             return {};
         }},
        switchVar("ORTHOMODE", "ortho on (1) or off (0)", drafting::keys::ortho,
                  &drafting::DraftingSettings::ortho),
        switchVar("SNAPMODE", "grid snap on (1) or off (0)", drafting::keys::snap,
                  &drafting::DraftingSettings::snap),
        switchVar("GRIDMODE", "grid shown (1) or hidden (0)", drafting::keys::grid,
                  &drafting::DraftingSettings::grid),
        {"TEXTSIZE", "height of new text (0 = suits the drawing and its scale)",
         [](const cmd::IHost& host) {
             const auto s = host.setting("text.height");
             return s && !s->empty() ? formatDouble(std::strtod(s->c_str(), nullptr))
                                     : std::string("0");
         },
         [](cmd::IHost& host, std::string_view v) -> std::string {
             const std::string t(v);
             char* end = nullptr;
             const double h = std::strtod(t.c_str(), &end);
             if (t.empty() || end != t.c_str() + t.size() || !(h >= 0) || !std::isfinite(h))
                 return "TEXTSIZE is a height of 0 or more.";
             host.setSetting("text.height", h > 0 ? formatDouble(h) : std::string());
             return {};
         }},
    };
    return vars;
}

const HostVar* findHostVar(std::string_view name) {
    for (const HostVar& v : hostVariables())
        if (name == v.name)
            return &v;
    return nullptr;
}

// Asks for the new value of a host variable and sets it.
Task askHostVar(CommandContext& ctx, const HostVar& v, const std::string& prompt) {
    cmd::StringOptions so;
    so.defaultValue = v.get(ctx.host());
    auto val = co_await ctx.getString(prompt + " <" + *so.defaultValue + ">", so);
    if (!val.ok())
        co_return;
    const std::string err = v.set(ctx.host(), cmd::trim(val.text));
    ctx.message(err.empty() ? std::string(v.name) + " = " + v.get(ctx.host()) : err);
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
            for (const HostVar& v : hostVariables())
                ctx.message(std::string(v.name) + " = " + v.get(ctx.host()) + "  (" + v.help + ")");
            co_return;
        }
        if (const HostVar* hv = findHostVar(name)) {
            co_await askHostVar(ctx, *hv, "New value for " + name);
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

namespace {
// A command named like one host variable (OSMODE, ORTHOMODE, ...).
class HostVarCommand final : public cmd::Command {
public:
    explicit HostVarCommand(const HostVar& v) : var_(v) {}
    std::string_view name() const override { return var_.name; }
    std::string_view description() const override { return var_.help; }
    // Usable in the middle of another command, as the switches they stand for.
    bool isTransparent() const override { return true; }
    Task run(CommandContext& ctx) override {
        co_await askHostVar(ctx, var_, "Enter new value for " + std::string(var_.name));
    }

private:
    const HostVar& var_;
};
}  // namespace

std::unique_ptr<cmd::Command> makeVarCommand(const std::string& name) {
    if (const HostVar* hv = findHostVar(name))
        return std::make_unique<HostVarCommand>(*hv);
    const auto it = std::find_if(variables().begin(), variables().end(),
                                 [&](const Var& v) { return name == v.name; });
    return std::make_unique<VarCommand>(name, it == variables().end() ? "" : it->help);
}

namespace {
// A 0/1 switch kept as a setting of this installation, not of the drawing: LWDISPLAY (the LWT
// button of the status bar), QPMODE (Quick Properties), ROLLOVERTIPS (hover tooltips).
class SwitchSettingCommand final : public cmd::Command {
public:
    SwitchSettingCommand(std::string name, std::string key, bool fallback, std::string help)
        : name_(std::move(name)),
          key_(std::move(key)),
          fallback_(fallback),
          help_(std::move(help)) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return help_; }
    Task run(CommandContext& ctx) override {
        const auto cur = ctx.host().setting(key_);
        const bool on = cur ? (*cur == "1" || *cur == "true") : fallback_;
        cmd::StringOptions so;
        so.defaultValue = on ? "1" : "0";
        auto val = co_await ctx.getString(
            "Enter new value for " + name_ + " <" + *so.defaultValue + ">", so);
        if (!val.ok())
            co_return;
        bool want = false;
        if (!parseSwitch(cmd::trim(val.text), want)) {
            ctx.message(name_ + " is 0 or 1.");
            co_return;
        }
        ctx.host().setSetting(key_, want ? "1" : "0");
        ctx.message(name_ + " = " + (want ? "1" : "0"));
    }

private:
    std::string name_, key_;
    bool fallback_;
    std::string help_;
};
}  // namespace

namespace {
// A whole number kept as a setting of this installation: PICKBOX, CURSORSIZE, GRIPSIZE, ... The
// Options dialog changes the same settings.
class NumberSettingCommand final : public cmd::Command {
public:
    NumberSettingCommand(std::string name, std::string key, int lo, int hi, int fallback,
                         std::string help)
        : name_(std::move(name)),
          key_(std::move(key)),
          lo_(lo),
          hi_(hi),
          fallback_(fallback),
          help_(std::move(help)) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return help_; }
    Task run(CommandContext& ctx) override {
        int cur = fallback_;
        if (const auto v = ctx.host().setting(key_); v && !v->empty()) {
            char* end = nullptr;
            const double d = std::strtod(v->c_str(), &end);
            if (end != v->c_str())
                cur = static_cast<int>(std::lround(d));
        }
        cmd::StringOptions so;
        so.defaultValue = std::to_string(cur);
        auto val = co_await ctx.getString(
            "Enter new value for " + name_ + " <" + *so.defaultValue + ">", so);
        if (!val.ok())
            co_return;
        const std::string text(cmd::trim(val.text));
        char* end = nullptr;
        const long n = std::strtol(text.c_str(), &end, 10);
        if (text.empty() || *end != 0 || n < lo_ || n > hi_) {
            ctx.message(name_ + " is a whole number from " + std::to_string(lo_) + " to " +
                        std::to_string(hi_) + ".");
            co_return;
        }
        ctx.host().setSetting(key_, std::to_string(n));
        ctx.message(name_ + " = " + std::to_string(n));
    }

private:
    std::string name_, key_;
    int lo_, hi_, fallback_;
    std::string help_;
};

// SAVETIME: minutes between automatic saves; 0 switches them off.
class SaveTimeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SAVETIME"; }
    std::string_view description() const override {
        return "Minutes between automatic saves (0 = off)";
    }
    Task run(CommandContext& ctx) override {
        const auto on = ctx.host().setting("autosave.enabled");
        const auto mins = ctx.host().setting("autosave.minutes");
        const bool enabled = !on || (*on == "1" || *on == "true");
        const long cur =
            enabled ? (mins ? std::lround(std::strtod(mins->c_str(), nullptr)) : 10) : 0;
        cmd::StringOptions so;
        so.defaultValue = std::to_string(cur);
        auto val =
            co_await ctx.getString("Enter new value for SAVETIME <" + *so.defaultValue + ">", so);
        if (!val.ok())
            co_return;
        const std::string text(cmd::trim(val.text));
        char* end = nullptr;
        const long n = std::strtol(text.c_str(), &end, 10);
        if (text.empty() || *end != 0 || n < 0 || n > 600) {
            ctx.message("SAVETIME is a whole number of minutes from 0 to 600.");
            co_return;
        }
        ctx.host().setSetting("autosave.enabled", n > 0 ? "1" : "0");
        if (n > 0)
            ctx.host().setSetting("autosave.minutes", std::to_string(n));
        ctx.message("SAVETIME = " + std::to_string(n));
    }
};

// SHORTCUTMENU: what a right-click in the drawing does, as a bit code (see radial_wheel.h of
// the ui library, which reads the same three settings).
class ShortcutMenuCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SHORTCUTMENU"; }
    std::string_view description() const override {
        return "Right-click menus: 1 default, 2 edit, 4 command, 8 command only with options";
    }
    Task run(CommandContext& ctx) override {
        auto get = [&](const char* key) { return ctx.host().setting(key).value_or("menu"); };
        int cur = 0;
        if (get("ui.rightClick") != "enter") {
            if (get("ui.rightClick.default") != "repeat")
                cur |= 1;
            if (get("ui.rightClick.edit") != "repeat")
                cur |= 2;
            const std::string c = get("ui.rightClick.command");
            cur |= c == "options" ? 8 : c == "enter" ? 0 : 4;
        }
        cmd::StringOptions so;
        so.defaultValue = std::to_string(cur);
        auto val = co_await ctx.getString(
            "Enter new value for SHORTCUTMENU <" + *so.defaultValue + ">", so);
        if (!val.ok())
            co_return;
        const std::string text(cmd::trim(val.text));
        char* end = nullptr;
        const long n = std::strtol(text.c_str(), &end, 10);
        if (text.empty() || *end != 0 || n < 0 || n > 31) {
            ctx.message("SHORTCUTMENU is a bit code from 0 to 15.");
            co_return;
        }
        ctx.host().setSetting("ui.rightClick", "menu");
        ctx.host().setSetting("ui.rightClick.default", (n & 1) ? "menu" : "repeat");
        ctx.host().setSetting("ui.rightClick.edit", (n & 2) ? "menu" : "repeat");
        ctx.host().setSetting("ui.rightClick.command", (n & 4)   ? "menu"
                                                       : (n & 8) ? "options"
                                                                 : "enter");
        ctx.message("SHORTCUTMENU = " + std::to_string(n & 15));
    }
};
}  // namespace

std::unique_ptr<cmd::Command> makeOptionVarCommand(std::string_view name) {
    struct Spec {
        const char* name;
        const char* key;
        int lo, hi, fallback;
        const char* help;
    };
    static constexpr Spec kSpecs[] = {
        {"PICKBOX", "pick.aperturePx", 1, 50, 6, "Size of the pick box, pixels"},
        {"APERTURE", "drafting.osnap.aperture", 1, 50, 10, "Object snap aperture, pixels"},
        {"CURSORSIZE", "view.crosshairSize", 1, 100, 100, "Crosshair size, % of the drawing area"},
        {"GRIPSIZE", "grips.size", 1, 20, 4, "Grip size, pixels from the centre to the edge"},
        {"GRIPCOLOR", "grips.color", 1, 255, 150, "Colour number of unselected grips"},
        {"GRIPHOVER", "grips.hover", 1, 255, 11, "Colour number of the grip under the cursor"},
        {"GRIPHOT", "grips.hot", 1, 255, 12, "Colour number of selected (hot) grips"},
        {"LAYLOCKFADECTL", "view.lockFade", -90, 90, 60,
         "How much objects on locked layers fade, % (0 or less: not at all)"},
        {"TEXTEDITMODE", "textedit.mode", 0, 1, 0,
         "TEXTEDIT asks for another object after each edit (0) or ends (1)"},
        {"ISAVEPERCENT", "files.isavePercent", 0, 100, 50,
         "Allowed waste in a saved file, % (every save writes a whole new file, so this has no "
         "effect yet)"},
    };
    if (name == "SAVETIME")
        return std::make_unique<SaveTimeCommand>();
    if (name == "SHORTCUTMENU")
        return std::make_unique<ShortcutMenuCommand>();
    if (name == "ISAVEBAK")
        return std::make_unique<SwitchSettingCommand>(
            "ISAVEBAK", "files.backup", true,
            "Keep the previous file as .bak when saving (0 or 1)");
    if (name == "AUDITCTL")
        return std::make_unique<SwitchSettingCommand>(
            "AUDITCTL", "audit.report", false,
            "AUDIT and RECOVER also write a report file next to the drawing (0 or 1)");
    if (name == "APBOX")
        return std::make_unique<SwitchSettingCommand>(
            "APBOX", "osnap.apbox", false,
            "Show the object snap aperture box at point prompts (0 or 1)");
    for (const Spec& s : kSpecs)
        if (name == s.name)
            return std::make_unique<NumberSettingCommand>(s.name, s.key, s.lo, s.hi, s.fallback,
                                                          s.help);
    return nullptr;
}

std::unique_ptr<cmd::Command> makeLwDisplayCommand() {
    return std::make_unique<SwitchSettingCommand>(
        "LWDISPLAY", "view.lineweights", true,
        "Show lineweights on screen (On/Off, same as the LWT button)");
}

std::unique_ptr<cmd::Command> makeQpModeCommand() {
    return std::make_unique<SwitchSettingCommand>(
        "QPMODE", "ui.quickProps", false, "Show Quick Properties for the selection (0 or 1)");
}

std::unique_ptr<cmd::Command> makeRolloverTipsCommand() {
    return std::make_unique<SwitchSettingCommand>(
        "ROLLOVERTIPS", "hover.tooltip", true,
        "Show an object's type, layer, colour and size when the pointer rests on it (0 or 1)");
}

std::unique_ptr<cmd::Command> makeSetvarCommand() {
    return std::make_unique<SetvarCommand>();
}

std::optional<std::string> variableValue(const Document& doc, std::string_view name) {
    const std::string n = upper(std::string(name));
    for (const Var& v : variables())
        if (n == v.name)
            return v.get(doc);
    return std::nullopt;
}

std::string setVariable(Transaction& tx, const Document& doc, std::string_view name,
                        std::string_view text) {
    const std::string n = upper(std::string(name));
    for (const Var& v : variables()) {
        if (n != v.name)
            continue;
        std::string err;
        auto r = tx.modifyHeader([&](Header& h) { err = v.set(doc, text, h); });
        if (!r)
            return r.error().message;
        return err;
    }
    return "Unknown variable " + n + ".";
}

}  // namespace tbcad::sysvars
