#include <algorithm>
#include <cstdio>

#include "module_state.h"
#include "tbcad/draftingcmds/drafting_cmds.h"

namespace tbcad::draftingcmds {

void registerUcsCommands(cmd::Registry& registry);

namespace {

using drafting::keys::dynInput;
using drafting::keys::grid;
using drafting::keys::ortho;
using drafting::keys::osnap;
using drafting::keys::polar;
using drafting::keys::snap;

// Current state of one switch, with the same defaults the filters use (OSNAP starts on).
bool isOn(const cmd::IHost& host, const char* key) {
    const auto s = drafting::DraftingSettings::read(host);
    const std::string_view k = key;
    if (k == osnap)
        return s.osnap;
    if (k == grid)
        return s.grid;
    if (k == snap)
        return s.snap;
    if (k == ortho)
        return s.ortho;
    if (k == drafting::keys::dynUcs)
        return s.dynUcs;
    if (k == dynInput)
        return s.dynInput;
    if (k == drafting::keys::otrack)
        return s.otrack;
    return s.polar;
}

void setOn(cmd::CommandContext& ctx, const char* key, bool on) {
    ctx.host().setSetting(key, on ? "1" : "0");
}

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// ORTHO and POLAR exclude each other: switching one on switches the other off.
void setOnExclusive(cmd::CommandContext& ctx, const char* key, bool on) {
    setOn(ctx, key, on);
    if (on && std::string_view(key) == ortho)
        setOn(ctx, polar, false);
    if (on && std::string_view(key) == polar)
        setOn(ctx, ortho, false);
}

// Shared shape of OSNAP, GRID, SNAP, ORTHO and POLAR: an ON/OFF/Toggle switch plus a few
// option keywords. Enter toggles.
class SwitchCommand : public cmd::Command {
public:
    SwitchCommand(std::string name, std::string label, const char* key, std::string description,
                  std::vector<std::string> aliases = {})
        : name_(std::move(name)),
          label_(std::move(label)),
          key_(key),
          description_(std::move(description)),
          aliases_(std::move(aliases)) {}

    std::string_view name() const override { return name_; }
    std::vector<std::string> defaultAliases() const override { return aliases_; }
    std::string_view description() const override { return description_; }
    // Usable in the middle of another command: 'ORTHO while LINE waits for a point.
    bool isTransparent() const override { return true; }

    cmd::Task run(cmd::CommandContext& ctx) override {
        std::vector<std::string> words = {"ON", "OFF", "Toggle"};
        for (const std::string& o : options())
            words.push_back(o);
        cmd::KeywordOptions ko;
        ko.keywords = words;
        ko.defaultKeyword = "Toggle";
        auto r = co_await ctx.getKeyword(label_ + " [" + join(words) + "]", ko);
        if (!r.ok())
            co_return;
        const bool now = isOn(ctx.host(), key_);
        if (r.keyword == "ON" || r.keyword == "OFF" || r.keyword == "Toggle") {
            const bool on = r.keyword == "ON" ? true : r.keyword == "OFF" ? false : !now;
            setOnExclusive(ctx, key_, on);
            ctx.message(label_ + (on ? " on" : " off"));
            co_return;
        }
        co_await option(ctx, r.keyword);
    }

protected:
    virtual std::vector<std::string> options() const { return {}; }
    virtual cmd::Task option(cmd::CommandContext&, const std::string&) { co_return; }

    static std::string join(const std::vector<std::string>& w) {
        std::string s;
        for (const auto& x : w) {
            if (!s.empty())
                s += '/';
            s += x;
        }
        return s;
    }

    std::string name_, label_;
    const char* key_;
    std::string description_;
    std::vector<std::string> aliases_;
};

class OsnapCommand final : public SwitchCommand {
public:
    OsnapCommand()
        : SwitchCommand("OSNAP", "Object snap", osnap, "Object snap on/off and modes", {"OS"}) {}

protected:
    std::vector<std::string> options() const override { return {"Modes", "Aperture"}; }
    cmd::Task option(cmd::CommandContext& ctx, const std::string& kw) override {
        if (kw == "Modes") {
            const auto cur = drafting::DraftingSettings::read(ctx.host());
            cmd::StringOptions so;
            so.defaultValue = drafting::formatSnapModes(cur.modes);
            auto m = co_await ctx.getString(
                "Snap modes (endpoint, midpoint, center, intersection, perpendicular, tangent, "
                "nearest, quadrant, node, insertion, apparent, extension, all, none)",
                so);
            if (!m.ok())
                co_return;
            auto mask = drafting::parseSnapModes(m.text);
            if (!mask) {
                ctx.message("Unknown snap mode in \"" + m.text + "\".");
                co_return;
            }
            ctx.host().setSetting(drafting::keys::osnapModes, drafting::formatSnapModes(*mask));
            ctx.message("Snap modes: " + drafting::formatSnapModes(*mask));
        } else if (kw == "Aperture") {
            cmd::RealOptions ro;
            ro.min = 1;
            ro.max = 50;
            ro.defaultValue = drafting::DraftingSettings::read(ctx.host()).aperturePx;
            auto a = co_await ctx.getReal("Snap aperture in pixels", ro);
            if (!a.ok())
                co_return;
            ctx.host().setSetting(drafting::keys::osnapAperture, number(a.value));
            ctx.message("Snap aperture: " + number(a.value) + " px");
        }
    }
};

class GridCommand final : public SwitchCommand {
public:
    GridCommand() : SwitchCommand("GRID", "Grid", grid, "Grid display on/off, spacing and style") {}

protected:
    std::vector<std::string> options() const override { return {"Spacing", "Style"}; }
    cmd::Task option(cmd::CommandContext& ctx, const std::string& kw) override {
        if (kw == "Spacing") {
            cmd::RealOptions ro;
            ro.min = 1e-6;
            ro.defaultValue = drafting::DraftingSettings::read(ctx.host()).gridSpacing;
            auto s = co_await ctx.getReal("Grid spacing", ro);
            if (!s.ok())
                co_return;
            ctx.host().setSetting(drafting::keys::gridSpacing, number(s.value));
            setOn(ctx, grid, true);
            ctx.message("Grid spacing: " + number(s.value));
        } else if (kw == "Style") {
            cmd::KeywordOptions ko;
            ko.keywords = {"Dots", "Lines"};
            ko.defaultKeyword =
                drafting::DraftingSettings::read(ctx.host()).gridLines ? "Lines" : "Dots";
            auto k = co_await ctx.getKeyword("Grid style [Dots/Lines]", ko);
            if (!k.ok())
                co_return;
            ctx.host().setSetting(drafting::keys::gridStyle,
                                  k.keyword == "Lines" ? "lines" : "dots");
        }
    }
};

class SnapCommand final : public SwitchCommand {
public:
    SnapCommand() : SwitchCommand("SNAP", "Snap", snap, "Grid snap on/off and spacing") {}

protected:
    std::vector<std::string> options() const override { return {"Spacing"}; }
    cmd::Task option(cmd::CommandContext& ctx, const std::string&) override {
        cmd::RealOptions ro;
        ro.min = 1e-6;
        ro.defaultValue = drafting::DraftingSettings::read(ctx.host()).snapSpacing;
        auto s = co_await ctx.getReal("Snap spacing", ro);
        if (!s.ok())
            co_return;
        ctx.host().setSetting(drafting::keys::snapSpacing, number(s.value));
        setOn(ctx, snap, true);
        ctx.message("Snap spacing: " + number(s.value));
    }
};

class OrthoCommand final : public SwitchCommand {
public:
    OrthoCommand() : SwitchCommand("ORTHO", "Ortho", ortho, "Ortho mode on/off") {}
};

class PolarCommand final : public SwitchCommand {
public:
    PolarCommand()
        : SwitchCommand("POLAR", "Polar tracking", polar, "Polar tracking on/off and angles") {}

protected:
    std::vector<std::string> options() const override {
        return {"Increment", "Additional", "Relative"};
    }
    cmd::Task option(cmd::CommandContext& ctx, const std::string& opt) override {
        if (opt == "Additional") {
            // Single angles tracked as they are, like AutoCAD's additional polar angles.
            cmd::StringOptions so;
            so.allowEmpty = true;
            auto a = co_await ctx.getString(
                "Additional angles in degrees (a list like 17,127.5; empty for none)", so);
            if (!a.ok())
                co_return;
            if (!drafting::parsePolarAngles(a.text)) {
                ctx.message("Invalid angles \"" + a.text + "\"; use numbers between -360 and 360.");
                co_return;
            }
            ctx.host().setSetting(drafting::keys::polarAdditional, a.text);
            setOnExclusive(ctx, polar, true);
            ctx.message(a.text.empty() ? std::string("No additional polar angles")
                                       : "Additional polar angles: " + a.text);
            co_return;
        }
        if (opt == "Relative") {
            // Measure polar angles from the last segment (Relative) or the UCS X axis (Absolute).
            const bool rel = drafting::DraftingSettings::read(ctx.host()).polarRelative;
            cmd::KeywordOptions ko;
            ko.keywords = {"Absolute", "Relative"};
            ko.defaultKeyword = rel ? "Relative" : "Absolute";
            auto k =
                co_await ctx.getKeyword("Measure polar angles [Absolute/Relative]", std::move(ko));
            if (!k.ok())
                co_return;
            const bool want = k.keyword == "Relative";
            ctx.host().setSetting(drafting::keys::polarRelative, want ? "1" : "0");
            ctx.message(want ? "Polar angles relative to the last segment"
                             : "Polar angles absolute");
            co_return;
        }
        cmd::StringOptions so;
        so.defaultValue = "90";
        auto a = co_await ctx.getString(
            "Increment angles in degrees (90, 45, 30, 15 or a list like 30,45)", so);
        if (!a.ok())
            co_return;
        auto inc = drafting::parsePolarIncrements(a.text);
        if (inc.empty()) {
            ctx.message("Invalid increment \"" + a.text + "\"; use numbers between 0 and 180.");
            co_return;
        }
        ctx.host().setSetting(drafting::keys::polarIncrement, a.text);
        setOnExclusive(ctx, polar, true);
        ctx.message("Polar tracking on, increment " + a.text);
    }
};

// ISODRAFT: isometric drafting on/off. Ortho, snap and the grid follow the axes of the current
// isometric plane (ISOPLANE).
class IsoDraftCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ISODRAFT"; }
    std::string_view description() const override {
        return "Isometric drafting on/off (ortho, snap and grid follow the isometric axes)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const bool on =
            drafting::DraftingSettings::read(ctx.host()).isoPlane != drafting::IsoPlane::Off;
        cmd::KeywordOptions ko;
        ko.keywords = {"ON", "OFF"};
        ko.defaultKeyword = on ? "OFF" : "ON";
        auto r = co_await ctx.getKeyword("Isometric drafting [ON/OFF]", std::move(ko));
        if (!r.ok())
            co_return;
        const bool want = cmd::toUpperAscii(r.keyword) == "ON";
        ctx.host().setSetting(drafting::keys::isoDraft, want ? "1" : "0");
        ctx.message(want ? "Isometric drafting on, plane " +
                               std::string(drafting::isoPlaneName(
                                   drafting::DraftingSettings::read(ctx.host()).isoPlane))
                         : std::string("Isometric drafting off"));
    }
};

// ISOPLANE [Left/Top/Right]: the isometric plane. Enter goes to the next one (the F5 key).
class IsoPlaneCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "ISOPLANE"; }
    bool isTransparent() const override { return true; }
    std::string_view description() const override {
        return "Set the isometric plane: Left, Top or Right (Enter = next)";
    }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto cur = drafting::DraftingSettings::read(ctx.host()).isoPlane;
        if (cur == drafting::IsoPlane::Off)
            cur = drafting::IsoPlane::Top;
        const auto next = drafting::nextIsoPlane(cur);
        cmd::KeywordOptions ko;
        ko.keywords = {"Left", "Top", "Right"};
        ko.defaultKeyword = std::string(drafting::isoPlaneName(next));
        auto r = co_await ctx.getKeyword("Current isometric plane is " +
                                             std::string(drafting::isoPlaneName(cur)) +
                                             ". Enter isometric plane setting [Left/Top/Right]",
                                         std::move(ko));
        if (!r.ok())
            co_return;
        const auto plane = drafting::parseIsoPlane(r.keyword).value_or(next);
        ctx.host().setSetting(drafting::keys::isoPlane, std::string(drafting::isoPlaneName(plane)));
        ctx.host().setSetting(drafting::keys::isoDraft, "1");
        ctx.message("Current isometric plane is " + std::string(drafting::isoPlaneName(plane)));
    }
};

// Hidden one-keystroke toggles used by the F-keys. No prompt, so they also work while another
// command waits for a point (typed as 'TOGGLEORTHO by the menu items).
class ToggleCommand final : public cmd::Command {
public:
    ToggleCommand(std::string name, std::string label, const char* key)
        : name_(std::move(name)), label_(std::move(label)), key_(key) {}
    std::string_view name() const override { return name_; }
    bool isTransparent() const override { return true; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        const bool on = !isOn(ctx.host(), key_);
        setOnExclusive(ctx, key_, on);
        ctx.message(label_ + (on ? " on" : " off"));
        co_return;
    }

private:
    std::string name_, label_;
    const char* key_;
};

// Tab: next dynamic input field while there are some, else the next object snap candidate under
// the cursor.
class SnapCycleCommand final : public cmd::Command {
public:
    std::string_view name() const override { return "SNAPCYCLE"; }
    bool isTransparent() const override { return true; }
    cmd::Task run(cmd::CommandContext&) override {
        // With dynamic input fields on the screen Tab goes through them instead.
        if (auto* d = moduleState().dyn; d && d->focusNext())
            co_return;
        if (auto* f = moduleState().osnap)
            f->cycle();
        co_return;
    }
};

}  // namespace

void registerCommands(cmd::Registry& registry) {
    (void)registry.add(std::make_unique<OsnapCommand>());
    (void)registry.add(std::make_unique<GridCommand>());
    (void)registry.add(std::make_unique<SnapCommand>());
    (void)registry.add(std::make_unique<OrthoCommand>());
    (void)registry.add(std::make_unique<PolarCommand>());
    (void)registry.add(std::make_unique<IsoDraftCommand>());
    (void)registry.add(std::make_unique<IsoPlaneCommand>());
    (void)registry.add(std::make_unique<ToggleCommand>("TOGGLEOSNAP", "Object snap", osnap));
    (void)registry.add(std::make_unique<ToggleCommand>("TOGGLEGRID", "Grid", grid));
    (void)registry.add(std::make_unique<ToggleCommand>("TOGGLESNAP", "Snap", snap));
    (void)registry.add(std::make_unique<ToggleCommand>("TOGGLEORTHO", "Ortho", ortho));
    (void)registry.add(std::make_unique<ToggleCommand>("TOGGLEPOLAR", "Polar tracking", polar));
    (void)registry.add(std::make_unique<ToggleCommand>("TOGGLEOTRACK", "Object snap tracking",
                                                       drafting::keys::otrack));
    (void)registry.add(
        std::make_unique<ToggleCommand>("DYNUCS", "Dynamic UCS", drafting::keys::dynUcs));
    (void)registry.add(
        std::make_unique<ToggleCommand>("TOGGLEDYNINPUT", "Dynamic input", dynInput));
    (void)registry.add(std::make_unique<SnapCycleCommand>());
    registerUcsCommands(registry);
}

}  // namespace tbcad::draftingcmds
