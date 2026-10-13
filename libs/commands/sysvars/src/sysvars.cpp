// COLOR, LWEIGHT, LTSCALE, UNITS, STATUS, REGEN and REDRAW. The first three set what new objects
// get (the "current" properties); every change is one undo step.
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

class UnitsCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "UNITS"; }
    std::vector<std::string> defaultAliases() const override { return {"UN"}; }
    std::string_view description() const override {
        return "Choose the length unit of the drawing (objects are not scaled)";
    }
    Task run(CommandContext& ctx) override {
        cmd::KeywordOptions ko;
        ko.keywords = {"Millimeters", "Centimeters", "Meters", "Inches", "Feet"};
        ko.defaultKeyword = unitName(ctx.doc().header().units);
        auto k = co_await ctx.getKeyword(
            "Length unit [Millimeters/Centimeters/Meters/Inches/Feet] <" + *ko.defaultKeyword + ">",
            ko);
        if (!k.ok())
            co_return;
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
    Task run(CommandContext& ctx) override {
        ctx.message("The display is always up to date.");
        co_return;
    }

private:
    const char* name_;
    const char* alias_;
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<ColorCommand>());
    (void)registry.add(std::make_unique<LineweightCommand>());
    (void)registry.add(std::make_unique<LtscaleCommand>());
    (void)registry.add(std::make_unique<UnitsCommand>());
    (void)registry.add(std::make_unique<StatusCommand>());
    (void)registry.add(std::make_unique<RefreshCommand>("REGEN", "RE"));
    (void)registry.add(std::make_unique<RefreshCommand>("REDRAW", "R"));
    (void)registry.add(makeRenameCommand());
}

}  // namespace tbcad::sysvars
