// COLOR, LWEIGHT, LTSCALE, UNITS, STATUS, REGEN and REDRAW. The first three set what new objects
// get (the "current" properties); every change is one undo step.
#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>

#include "commands.h"
#include "tbcad/layers/layer_ops.h"

namespace tbcad::sysvars {

namespace {

using cmd::CommandContext;
using cmd::Task;

class ColorCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "COLOR"; }
    std::vector<std::string> defaultAliases() const override { return {"COL"}; }
    std::string_view description() const override { return "Set the colour of new objects"; }
    Task run(CommandContext& ctx) override {
        cmd::StringOptions so;
        so.defaultValue = layers::colorName(ctx.doc().header().currentColor);
        auto in = co_await ctx.getString(
            "New object colour (name, 1-255, r,g,b, ByLayer, ByBlock) <" + *so.defaultValue + ">",
            so);
        if (!in.ok())
            co_return;
        auto color = layers::parseColor(cmd::trim(in.text));
        if (!color) {
            ctx.message(color.error().message);
            co_return;
        }
        if (color.value().kind == ColorKind::ByParent) {
            ctx.message("ByParent only applies to sub-layers.");
            co_return;
        }
        ctx.setUndoLabel("Colour");
        const Color c = color.value();
        if (auto r = ctx.tx().modifyHeader([c](Header& h) { h.currentColor = c; }); !r)
            ctx.message(r.error().message);
        else
            ctx.message("New objects use the colour " + layers::colorName(c) + ".");
    }
};

class LineweightCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LWEIGHT"; }
    std::vector<std::string> defaultAliases() const override { return {"LW"}; }
    std::string_view description() const override { return "Set the lineweight of new objects"; }
    Task run(CommandContext& ctx) override {
        cmd::StringOptions so;
        so.defaultValue = layers::lineweightName(ctx.doc().header().currentLineweight);
        auto in = co_await ctx.getString(
            "New object lineweight (mm, Default, ByLayer, ByBlock) <" + *so.defaultValue + ">", so);
        if (!in.ok())
            co_return;
        auto w = layers::parseLineweight(cmd::trim(in.text));
        if (!w) {
            ctx.message(w.error().message);
            co_return;
        }
        if (w.value().isByParent()) {
            ctx.message("ByParent only applies to sub-layers.");
            co_return;
        }
        ctx.setUndoLabel("Lineweight");
        const LineWeight lw = w.value();
        if (auto r = ctx.tx().modifyHeader([lw](Header& h) { h.currentLineweight = lw; }); !r)
            ctx.message(r.error().message);
        else
            ctx.message("New objects use the lineweight " + layers::lineweightName(lw) + ".");
    }
};

class TransparencyDefaultCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "CETRANSPARENCY"; }
    std::string_view description() const override { return "Set the transparency of new objects"; }
    Task run(CommandContext& ctx) override {
        cmd::StringOptions so;
        so.defaultValue = layers::transparencyName(ctx.doc().header().currentTransparency);
        auto in = co_await ctx.getString(
            "New object transparency (ByLayer, ByBlock or 0 to 90) <" + *so.defaultValue + ">", so);
        if (!in.ok())
            co_return;
        auto t = layers::parseTransparency(cmd::trim(in.text));
        if (!t) {
            ctx.message(t.error().message);
            co_return;
        }
        ctx.setUndoLabel("Transparency");
        const auto v = static_cast<std::int8_t>(t.value());
        if (auto r = ctx.tx().modifyHeader([v](Header& h) { h.currentTransparency = v; }); !r)
            ctx.message(r.error().message);
        else
            ctx.message("New objects use the transparency " + layers::transparencyName(v) + ".");
    }
};

class LtscaleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LTSCALE"; }
    std::vector<std::string> defaultAliases() const override { return {"LTS"}; }
    std::string_view description() const override {
        return "Set the scale of the dashes in linetypes";
    }
    Task run(CommandContext& ctx) override {
        cmd::RealOptions ro;
        ro.defaultValue = ctx.doc().header().ltScale;
        ro.min = 1e-6;
        ro.max = 1e6;
        auto v = co_await ctx.getReal("New linetype scale factor", ro);
        if (!v.ok())
            co_return;
        ctx.setUndoLabel("Linetype scale");
        const double s = v.value;
        if (auto r = ctx.tx().modifyHeader([s](Header& h) { h.ltScale = s; }); !r)
            ctx.message(r.error().message);
    }
};

const char* unitName(LengthUnit u) {
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

// UNITS opens the Drawing Units dialog; -UNITS is the typed form.
class UnitsDialogCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "UNITS"; }
    std::vector<std::string> defaultAliases() const override { return {"UN"}; }
    std::string_view description() const override {
        return "Open the Drawing Units dialog: length and angle format, precision, direction";
    }
    Task run(CommandContext& ctx) override {
        ctx.host().openPanel("units");
        co_return;
    }
};

std::string lowerFirst(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class UnitsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "-UNITS"; }
    std::vector<std::string> defaultAliases() const override { return {"DWGUNITS"}; }
    std::string_view description() const override {
        return "Choose the unit, length and angle format, precision and angle direction";
    }
    Task run(CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Millimeters", "Centimeters", "Meters", "Inches",    "Feet",
                       "Precision",   "Type",        "Angles", "Direction", "Base"};
        ko.defaultKeyword = unitName(ctx.doc().header().units);
        auto k = co_await ctx.getKeyword(
            "Length unit [Millimeters/Centimeters/Meters/Inches/Feet/"
            "Precision/Type/Angles/Direction/Base] <" +
                *ko.defaultKeyword + ">",
            ko);
        if (!k.ok())
            co_return;
        if (k.keyword == "Type" || k.keyword == "Angles") {
            const bool lengths = k.keyword == "Type";
            static const std::vector<std::string> kLength = {"Scientific", "Decimal", "Engineering",
                                                             "Architectural", "Fractional"};
            static const std::vector<std::string> kAngle = {"Degrees", "DMS", "Grads", "Radians",
                                                            "Surveyor"};
            const Header& h = ctx.doc().header();
            cmd::KeywordOptions to;
            to.keywords = lengths ? kLength : kAngle;
            to.defaultKeyword = lengths ? kLength[std::size_t(lengthUnitsType(h) - 1)]
                                        : kAngle[std::size_t(angleUnitsType(h))];
            const std::string prompt = lengths ? "Length type [Scientific/Decimal/Engineering/"
                                                 "Architectural/Fractional]"
                                               : "Angle type [Degrees/DMS/Grads/Radians/Surveyor]";
            auto t = co_await ctx.getKeyword(prompt, to);
            if (!t.ok())
                co_return;
            const auto& list = lengths ? kLength : kAngle;
            const int index =
                static_cast<int>(std::find(list.begin(), list.end(), t.keyword) - list.begin());
            ctx.setUndoLabel("Units");
            (void)ctx.tx().modifyHeader([&](Header& hh) {
                if (lengths)
                    setLengthUnitsType(hh, index + 1);
                else
                    setAngleUnitsType(hh, index);
            });
            ctx.message((lengths ? "Lengths are shown as " : "Angles are shown as ") + t.keyword +
                        ".");
            co_return;
        }
        if (k.keyword == "Direction") {
            cmd::KeywordOptions dk;
            dk.keywords = {"Counterclockwise", "Clockwise"};
            dk.defaultKeyword =
                anglesClockwise(ctx.doc().header()) ? "Clockwise" : "Counterclockwise";
            auto d = co_await ctx.getKeyword("Measure angles [Counterclockwise/Clockwise]", dk);
            if (!d.ok())
                co_return;
            ctx.setUndoLabel("Units");
            (void)ctx.tx().modifyHeader(
                [&](Header& hh) { setAnglesClockwise(hh, d.keyword == "Clockwise"); });
            ctx.message("Angles are measured " + lowerFirst(d.keyword) + ".");
            co_return;
        }
        if (k.keyword == "Base") {
            cmd::RealOptions ro;
            ro.defaultValue = angleBase(ctx.doc().header()) * 180.0 / kPi;
            auto b = co_await ctx.getReal("Direction of angle 0, degrees from east", ro);
            if (!b.ok())
                co_return;
            ctx.setUndoLabel("Units");
            (void)ctx.tx().modifyHeader(
                [&](Header& hh) { setAngleBase(hh, b.value * kPi / 180.0); });
            ctx.message("Angle 0 points " + formatDouble(b.value) + " degrees from east.");
            co_return;
        }
        if (k.keyword == "Precision") {
            // Decimal places of lengths and angles in measurements and lists (LUPREC, AUPREC).
            const Header& h = ctx.doc().header();
            cmd::IntOptions lo;
            lo.min = 0;
            lo.max = 8;
            lo.defaultValue = linearPrecision(h);
            auto l = co_await ctx.getInteger(
                "Decimal places for lengths <" + std::to_string(linearPrecision(h)) + ">", lo);
            if (!l.ok())
                co_return;
            cmd::IntOptions ao = lo;
            ao.defaultValue = anglePrecision(h);
            auto a = co_await ctx.getInteger(
                "Decimal places for angles <" + std::to_string(anglePrecision(h)) + ">", ao);
            if (!a.ok())
                co_return;
            const int lp = static_cast<int>(l.value), ap = static_cast<int>(a.value);
            ctx.setUndoLabel("Units precision");
            if (auto r = ctx.tx().modifyHeader([&](Header& hh) {
                    setLinearPrecision(hh, lp);
                    setAnglePrecision(hh, ap);
                });
                !r)
                ctx.message(r.error().message);
            else
                ctx.message("Lengths show " + std::to_string(lp) + " and angles " +
                            std::to_string(ap) + " decimal places.");
            co_return;
        }
        LengthUnit u = LengthUnit::Millimeter;
        for (LengthUnit c : {LengthUnit::Millimeter, LengthUnit::Centimeter, LengthUnit::Meter,
                             LengthUnit::Inch, LengthUnit::Foot})
            if (k.keyword == unitName(c))
                u = c;
        ctx.setUndoLabel("Units");
        if (auto r = ctx.tx().modifyHeader([u](Header& h) { h.units = u; }); !r)
            ctx.message(r.error().message);
        else
            ctx.message("The drawing unit is " + std::string(unitName(u)) +
                        ". Existing objects were not scaled.");
    }
};

class StatusCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "STATUS"; }
    std::string_view description() const override { return "Show the settings of the drawing"; }
    Task run(CommandContext& ctx) override {
        const Document& doc = ctx.doc();
        const Header& h = doc.header();
        std::size_t entities = 0;
        doc.forEachEntity([&](const Entity&) { ++entities; });
        ctx.message("Objects: " + std::to_string(entities));
        ctx.message("Layers: " + std::to_string(doc.layers().size()) +
                    ", blocks: " + std::to_string(doc.blocks().size()) +
                    ", text styles: " + std::to_string(doc.textStyles().size()));
        ctx.message(std::string("Unit: ") + unitName(h.units));
        if (const Layer* l = doc.layer(h.currentLayer))
            ctx.message("Current layer: " + l->name);
        ctx.message("Current colour: " + layers::colorName(h.currentColor) +
                    ", linetype: " + layers::linetypeName(doc, h.currentLinetype) +
                    ", lineweight: " + layers::lineweightName(h.currentLineweight));
        ctx.message("Linetype scale: " + formatDouble(h.ltScale));
        co_return;
    }
};

// FLATTEN: TBCAD drawings keep no Z coordinate (DWG/DXF import drops it), so the only height an
// object can have is its 2D level; FLATTEN puts the selected objects on the default level.
class FlattenCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "FLATTEN"; }
    std::string_view description() const override {
        return "Remove the height of objects: put them on the default level";
    }
    Task run(CommandContext& ctx) override {
        auto sel = co_await ctx.getSelection("Select objects to flatten");
        if (!sel.ok())
            co_return;
        std::size_t n = 0;
        for (EntityId id : sel.ids) {
            const Entity* e = ctx.doc().find(id);
            if (!e || ctx.doc().ownerBlock(id) || e->common.level == kLevelDefault)
                continue;
            if (ctx.tx().modify(id, [](Entity& x) { x.common.level = kLevelDefault; }))
                ++n;
        }
        if (n == 0)
            ctx.message("TBCAD drawings are 2D: nothing to flatten.");
        else
            ctx.message(std::to_string(n) + (n == 1 ? " object" : " objects") +
                        " moved to the default level.");
        co_return;
    }
};

class TimeCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "TIME"; }
    std::string_view description() const override { return "Show the current date and time"; }
    Task run(CommandContext& ctx) override {
        const std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        char buf[64];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
        ctx.message(std::string("Current time: ") + buf);
        co_return;
    }
};

// The display always shows the current drawing; these exist so the usual habit still works.
class RefreshCommand final : public cmd::Command {
public:
    RefreshCommand(const char* name, const char* alias) : name_(name), alias_(alias) {}
    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override {
        return alias_[0] ? std::vector<std::string>{alias_} : std::vector<std::string>{};
    }
    std::string_view description() const override {
        return "Refresh the display (it is always up to date)";
    }
    bool isTransparent() const override { return true; }
    Task run(CommandContext& ctx) override {
        ctx.message("The display is always up to date.");
        co_return;
    }

private:
    const char* name_;
    const char* alias_;
};

// FILL [ON/OFF]: solid fills (hatches, solids, wide polylines) show or only their outlines do.
class FillCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "FILL"; }
    std::string_view description() const override { return "Show or hide solid fills"; }
    Task run(CommandContext& ctx) override {
        const bool cur = fillMode(ctx.doc().header());
        cmd::KeywordOptions ko;
        ko.keywords = {"ON", "OFF"};
        ko.defaultKeyword = cur ? "ON" : "OFF";
        auto k = co_await ctx.getKeyword(
            std::string("Enter mode [ON/OFF] <") + (cur ? "ON" : "OFF") + ">", std::move(ko));
        if (!k.ok())
            co_return;
        const bool on = k.keyword == "ON";
        ctx.setUndoLabel(on ? "Fill on" : "Fill off");
        if (auto r =
                ctx.tx().modifyHeader([on](Header& h) { h.custom[kFillModeKey] = on ? "1" : "0"; });
            !r)
            ctx.message(r.error().message);
    }
};

// ATTDISP [Normal/ON/OFF]: attributes as defined, all shown, or all hidden.
class AttDispCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ATTDISP"; }
    std::string_view description() const override { return "Show or hide block attributes"; }
    Task run(CommandContext& ctx) override {
        static const char* const names[] = {"Normal", "ON", "OFF"};
        const int cur = attDisplay(ctx.doc().header());
        cmd::KeywordOptions ko;
        ko.keywords = {"Normal", "ON", "OFF"};
        ko.defaultKeyword = names[cur];
        auto k = co_await ctx.getKeyword(
            std::string("Enter attribute visibility setting [Normal/ON/OFF] <") + names[cur] + ">",
            std::move(ko));
        if (!k.ok())
            co_return;
        const char* v = k.keyword == "ON" ? "1" : k.keyword == "OFF" ? "2" : "0";
        ctx.setUndoLabel("Attribute display");
        if (auto r = ctx.tx().modifyHeader([v](Header& h) { h.custom[kAttDispKey] = v; }); !r)
            ctx.message(r.error().message);
    }
};

// Commands whose job TBCAD does by itself: they only say so.
class NoteCommand final : public cmd::Command {
public:
    NoteCommand(const char* name, const char* description, const char* note)
        : name_(name), description_(description), note_(note) {}
    std::string_view name() const override { return name_; }
    std::string_view description() const override { return description_; }
    Task run(CommandContext& ctx) override {
        ctx.message(note_);
        co_return;
    }

private:
    const char* name_;
    const char* description_;
    const char* note_;
};

// QTEXT [ON/OFF]: text shows as boxes, which redraws large drawings of text quickly.
class QTextCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "QTEXT"; }
    std::string_view description() const override { return "Show text as boxes"; }
    Task run(CommandContext& ctx) override {
        const bool cur = quickText(ctx.doc().header());
        cmd::KeywordOptions ko;
        ko.keywords = {"ON", "OFF"};
        ko.defaultKeyword = cur ? "ON" : "OFF";
        auto k = co_await ctx.getKeyword(
            std::string("Enter mode [ON/OFF] <") + (cur ? "ON" : "OFF") + ">", std::move(ko));
        if (!k.ok())
            co_return;
        const bool on = k.keyword == "ON";
        ctx.setUndoLabel(on ? "Quick text on" : "Quick text off");
        if (auto r =
                ctx.tx().modifyHeader([on](Header& h) { h.custom[kQTextKey] = on ? "1" : "0"; });
            !r)
            ctx.message(r.error().message);
    }
};

// MULTIPLE: repeats a command until Esc.
class MultipleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "MULTIPLE"; }
    std::string_view description() const override { return "Repeat a command until Esc"; }
    Task run(CommandContext& ctx) override {
        for (;;) {
            auto n = co_await ctx.getString("Enter command name to repeat");
            if (!n.ok())
                co_return;
            const std::string text = cmd::trim(n.text);
            cmd::Command* c = ctx.registry().find(text);
            if (!c) {
                ctx.message("Unknown command \"" + text + "\".");
                continue;
            }
            if (c == ctx.registry().find("MULTIPLE")) {
                ctx.message("MULTIPLE cannot repeat itself.");
                continue;
            }
            if (!ctx.host().repeatCommand(std::string(c->name())))
                ctx.message("This window cannot repeat commands.");
            co_return;
        }
    }
};

// LIMITS: the drawing area. ZOOM All covers it; DXF keeps it in $LIMMIN / $LIMMAX.
class LimitsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "LIMITS"; }
    std::string_view description() const override { return "Set the drawing limits"; }
    Task run(CommandContext& ctx) override {
        const BBox2 cur = drawingLimits(ctx.doc().header());
        const Vec2 curMin = cur.empty() ? Vec2{0, 0} : cur.min;
        const Vec2 curMax = cur.empty() ? Vec2{420, 297} : cur.max;
        cmd::PointOptions po;
        po.keywords = {"ON", "OFF"};
        po.allowNone = true;
        char buf[200];
        std::snprintf(buf, sizeof buf, "Specify lower left corner or [ON/OFF] <%g,%g>", curMin.x,
                      curMin.y);
        auto first = co_await ctx.getPoint(buf, po);
        if (first.status == cmd::Status::Keyword) {
            const bool on = first.keyword == "ON";
            ctx.setUndoLabel("Limits check");
            if (auto r = ctx.tx().modifyHeader(
                    [on](Header& h) { h.custom[kLimCheckKey] = on ? "1" : "0"; });
                !r)
                ctx.message(r.error().message);
            co_return;
        }
        if (!first.ok() && first.status != cmd::Status::None)
            co_return;
        const Vec2 ll = first.ok() ? first.point : curMin;
        cmd::PointOptions p2;
        p2.base = ll;
        p2.allowNone = true;
        std::snprintf(buf, sizeof buf, "Specify upper right corner <%g,%g>", curMax.x, curMax.y);
        auto second = co_await ctx.getPoint(buf, p2);
        if (!second.ok() && second.status != cmd::Status::None)
            co_return;
        const Vec2 ur = second.ok() ? second.point : curMax;
        if (!(ll.x < ur.x) || !(ll.y < ur.y)) {
            ctx.message("The upper right corner must be above and to the right of the lower left.");
            co_return;
        }
        ctx.setUndoLabel("Limits");
        if (auto r = ctx.tx().modifyHeader([&](Header& h) { setDrawingLimits(h, ll, ur); }); !r)
            ctx.message(r.error().message);
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ColorCommand>());
    (void)registry.add(std::make_unique<LineweightCommand>());
    (void)registry.add(std::make_unique<TransparencyDefaultCommand>());
    (void)registry.add(std::make_unique<LtscaleCommand>());
    (void)registry.add(std::make_unique<UnitsCommand>());
    (void)registry.add(std::make_unique<UnitsDialogCommand>());
    (void)registry.add(std::make_unique<StatusCommand>());
    (void)registry.add(std::make_unique<TimeCommand>());
    (void)registry.add(std::make_unique<FlattenCommand>());
    (void)registry.add(std::make_unique<RefreshCommand>("REGEN", "RE"));
    (void)registry.add(std::make_unique<RefreshCommand>("REDRAW", "R"));
    (void)registry.add(std::make_unique<RefreshCommand>("REGENALL", ""));
    (void)registry.add(std::make_unique<RefreshCommand>("REDRAWALL", "RA"));
    (void)registry.add(std::make_unique<RefreshCommand>("REGENAUTO", ""));
    (void)registry.add(std::make_unique<LimitsCommand>());
    (void)registry.add(std::make_unique<FillCommand>());
    (void)registry.add(std::make_unique<NoteCommand>(
        "CONVERTPOLY", "Convert old-style polylines to the optimized form",
        "Polylines are always kept in the optimized form; there is nothing to convert."));
    (void)registry.add(std::make_unique<QTextCommand>());
    (void)registry.add(std::make_unique<NoteCommand>(
        "CONVERTCTB", "Convert a drawing to colour-dependent plot styles",
        "TBCAD plots with colour-dependent tables (CTB) only, so every drawing already uses them. "
        "Use PLOTSTYLE to pick or import a table."));
    (void)registry.add(std::make_unique<NoteCommand>(
        "CONVERTPSTYLES", "Convert a drawing to named plot styles",
        "Named plot style tables (STB) are not used. Drawings plot with a colour-dependent "
        "table; use PLOTSTYLE to pick or import one."));
    (void)registry.add(std::make_unique<NoteCommand>(
        "UPDATEFIELD", "Update fields",
        "Fields are worked out each time they are drawn, so they are always up to date."));
    (void)registry.add(std::make_unique<MultipleCommand>());
    (void)registry.add(std::make_unique<AttDispCommand>());
    (void)registry.add(makeRenameCommand());
    (void)registry.add(makeHyperlinkCommand());
    (void)registry.add(makeSpaceTransCommand());
    (void)registry.add(makeDelayCommand());
    (void)registry.add(makeScriptCommand(false));
    (void)registry.add(makeScriptCommand(true));
    (void)registry.add(makeResumeCommand());
    (void)registry.add(makeSetvarCommand());
    for (const char* v : {"CELTSCALE", "PSLTSCALE", "MSLTSCALE", "PLINEGEN", "ANNOAUTOSCALE",
                          "ANNOALLVISIBLE", "SELECTIONANNODISPLAY", "PICKSTYLE", "MIRRTEXT", "HPORIGIN"})
        (void)registry.add(makeVarCommand(v));
    (void)registry.add(makeLwDisplayCommand());
    (void)registry.add(makeQpModeCommand());
    (void)registry.add(makeRolloverTipsCommand());
    for (const char* n : {"PICKBOX", "APERTURE", "CURSORSIZE", "GRIPSIZE", "GRIPCOLOR", "GRIPHOVER",
                          "GRIPHOT", "LAYLOCKFADECTL", "SAVETIME", "ISAVEBAK", "APBOX",
                          "SHORTCUTMENU", "TEXTEDITMODE", "AUDITCTL"})
        (void)registry.add(makeOptionVarCommand(n));
    (void)registry.add(makeViewCommand());
    (void)registry.add(makeViewManagerCommand());
    (void)registry.add(makeVportsCommand());
    (void)registry.add(makeVportsDialogCommand());
    (void)registry.add(makeAddSelectedCommand());
}

}  // namespace tbcad::sysvars
