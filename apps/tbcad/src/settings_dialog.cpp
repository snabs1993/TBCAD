#include "settings_dialog.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "key_map.h"
#include "tbcad/cmd/input.h"
#include "tbcad/core/color.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/platform/keybindings.h"
#include "tbcad/platform/paths.h"
#include "tbcad/tmpl/tmpl.h"
#include "tbcad/ui/customize.h"
#include "tbcad/ui/radial_wheel.h"
#include "tbcad/ui/theme.h"

namespace tbcad::app {

namespace {

const ImVec4 kWarn(0.95f, 0.45f, 0.35f, 1.0f);
const ImVec4 kMuted(0.6f, 0.6f, 0.6f, 1.0f);

bool containsNoCase(const std::string& hay, const std::string& needle) {
    if (needle.empty())
        return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

// A colour kept as "#RRGGBB" in `key`; empty means `fallback` (the theme's). Default puts it back.
void hexColorSetting(ui::AppCore& core, const char* label, const char* key, Rgba fallback) {
    const auto cur = ui::parseHexColor(core.setting(key).value_or(""));
    const Rgba c = cur.value_or(fallback);
    float rgb[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
    ImGui::PushID(key);
    if (ImGui::ColorEdit3("##c", rgb, ImGuiColorEditFlags_NoInputs)) {
        auto byte = [](float f) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f));
        };
        core.setSetting(key, ui::toHexColor(Rgba{byte(rgb[0]), byte(rgb[1]), byte(rgb[2]), 255}));
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    if (cur) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Default"))
            core.setSetting(key, "");
    }
    ImGui::PopID();
}

// A colour number (1-255) kept in `key`, as GRIPCOLOR stores it; unset shows `fallbackAci`
// and draws the built-in colour.
void aciSetting(ui::AppCore& core, const char* label, const char* key, int fallbackAci) {
    const auto v = core.setting(key);
    const long stored = v ? std::strtol(v->c_str(), nullptr, 10) : 0;
    int n = stored >= 1 && stored <= 255 ? static_cast<int>(stored) : fallbackAci;
    ImGui::PushID(key);
    const Rgba c = paletteColor(static_cast<std::uint8_t>(n));
    ImGui::ColorButton("##swatch", ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f),
                       ImGuiColorEditFlags_NoTooltip);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt(label, &n) && n >= 1 && n <= 255)
        core.setSetting(key, std::to_string(n));
    ImGui::PopID();
}

// A whole-number setting on a slider, saved through the host so the variable reads it back.
void intSlider(ui::AppCore& core, const char* label, const char* key, int lo, int hi,
               int fallback) {
    int v = static_cast<int>(std::lround(core.settings().getNumber(key, fallback)));
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderInt(label, &v, lo, hi))
        core.setSetting(key, std::to_string(v));
}

}  // namespace

void SettingsDialog::draw(ui::AppCore& core) {
    auto pageFlags = [this](const char* name) {
        return selectPage_ == name ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (wantOpen_) {
        ImGui::OpenPopup("Settings");
        wantOpen_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    bool keepOpen = true;  // the title-bar X closes it, like Close and Esc
    open_ = ImGui::BeginPopupModal("Settings", &keepOpen, ImGuiWindowFlags_NoSavedSettings);
    if (!open_)
        return;
    if (!keepOpen) {
        capturing_.clear();
        ImGui::CloseCurrentPopup();
    }
    const bool capturingAtStart = !capturing_.empty();
    // The option pages scroll above the Close row, which stays in view however long they get.
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    auto page = [&](const char* name, void (SettingsDialog::*fn)(ui::AppCore&)) {
        if (!ImGui::BeginTabItem(name, nullptr, pageFlags(name)))
            return;
        if (ImGui::BeginChild("##page", ImVec2(0, -footer)))
            (this->*fn)(core);
        ImGui::EndChild();
        ImGui::EndTabItem();
    };
    if (ImGui::BeginTabBar("##settingsPages")) {
        page("General", &SettingsDialog::general);
        page("Display", &SettingsDialog::display);
        page("Drafting", &SettingsDialog::drafting);
        page("Files", &SettingsDialog::files);
        if (ImGui::BeginTabItem("Aliases", nullptr, pageFlags("Aliases"))) {
            aliases(core);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keybindings", nullptr, pageFlags("Keybindings"))) {
            keybindings(core);
            ImGui::EndTabItem();
        }
        page("Customize", &SettingsDialog::customize);
        ImGui::EndTabBar();
        selectPage_.clear();
    }
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(100, 0)) ||
        (!capturingAtStart && ImGui::IsKeyPressed(ImGuiKey_Escape))) {
        capturing_.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all settings...", ImVec2(170, 0)))
        ImGui::OpenPopup("Reset settings?");
    if (ImGui::BeginPopupModal(
            "Reset settings?", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(
            "Put every option on these pages back to its default?\n"
            "Key bindings, aliases, recent files and workspaces are kept.");
        if (ImGui::Button("Reset", ImVec2(100, 0))) {
            core.settings().resetToDefaults();
            if (hooks_.setThemeMode)
                hooks_.setThemeMode(0);  // dark, the default
            if (hooks_.scaleChanged)
                hooks_.scaleChanged();
            if (hooks_.settingsReset)
                hooks_.settingsReset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Changes are saved as you make them.");
    ImGui::EndPopup();
}

void SettingsDialog::general(ui::AppCore& core) {
    auto& s = core.settings();
    bool lw = s.getBool("view.lineweights", true);
    if (ImGui::Checkbox("Show line weights (LWDISPLAY)", &lw))
        s.setBool("view.lineweights", lw);
    rightClick(core);
    wheel(core);
}

// Right-click customization (AutoCAD's User Preferences page, SHORTCUTMENU): what a short
// right-click in the drawing does with nothing selected, with objects selected, and while a
// command runs.
void SettingsDialog::rightClick(ui::AppCore& core) {
    auto& s = core.settings();
    ImGui::SeparatorText("Right-click in the drawing");
    bool allEnter = s.getString("ui.rightClick", "menu") == "enter";
    if (ImGui::Checkbox("Right-click is always Enter (no menu)", &allEnter))
        s.setString("ui.rightClick", allEnter ? "enter" : "menu");
    ImGui::BeginDisabled(allEnter);
    auto choice = [&](const char* label, const char* key, const char* const* values,
                      const char* const* labels, int count) {
        const std::string cur = s.getString(key, values[0]);
        int idx = 0;
        for (int i = 0; i < count; ++i)
            if (cur == values[i])
                idx = i;
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo(label, labels[idx])) {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(labels[i], i == idx))
                    s.setString(key, values[i]);
            ImGui::EndCombo();
        }
    };
    static const char* const kIdle[] = {"menu", "repeat"};
    static const char* const kIdleLabel[] = {"Shortcut menu", "Repeat last command"};
    static const char* const kCmd[] = {"menu", "options", "enter"};
    static const char* const kCmdLabel[] = {"Shortcut menu", "Menu when there are options",
                                            "Enter"};
    choice("Nothing selected", "ui.rightClick.default", kIdle, kIdleLabel, 2);
    choice("Objects selected", "ui.rightClick.edit", kIdle, kIdleLabel, 2);
    choice("While a command runs", "ui.rightClick.command", kCmd, kCmdLabel, 3);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Shift + right-click at a point prompt always shows the snap overrides.");
}

// A list of command lines kept in the setting `key`, in order: each can move up or down or go;
// a new one is typed below and must start with a command or an alias.
void SettingsDialog::commandList(ui::AppCore& core, const char* key) {
    auto& s = core.settings();
    std::vector<std::string> items = s.getStrings(key);
    bool changed = false;
    ImGui::PushID(key);
    for (std::size_t i = 0; i < items.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(items[i].c_str());
        ImGui::SameLine(300);
        if (ImGui::SmallButton("Up") && i > 0) {
            std::swap(items[i], items[i - 1]);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Down") && i + 1 < items.size()) {
            std::swap(items[i], items[i + 1]);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (items.empty())
        ImGui::TextDisabled("(none)");
    std::string& draft = listDraft_[key];
    ImGui::SetNextItemWidth(260);
    const bool enter = ImGui::InputTextWithHint("##add", "Command, e.g. LINE or ZOOM E", &draft,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add") || enter) && !draft.empty()) {
        const std::string word = ui::commandLabel(draft);
        if (!core.registry().find(word)) {
            listMessage_ = "No command or alias named " + word + ".";
        } else {
            items.push_back(draft);
            draft.clear();
            listMessage_.clear();
            changed = true;
        }
    }
    if (changed)
        s.setStrings(key, items);
    ImGui::PopID();
}

// Settings > Customize (CUI): the Quick Access toolbar, the extra commands of the right-click
// menu and what a double click does on each type of object.
void SettingsDialog::customize(ui::AppCore& core) {
    auto& s = core.settings();
    ImGui::SeparatorText("Quick Access toolbar");
    ImGui::TextDisabled("Buttons at the end of the toolbar; each runs its command line.");
    commandList(core, ui::kQuickAccessKey);

    ImGui::SeparatorText("Right-click menu");
    ImGui::TextUnformatted("Nothing selected:");
    commandList(core, ui::kContextMenuKey);
    ImGui::TextUnformatted("Objects selected:");
    commandList(core, ui::kContextMenuEditKey);
    if (!listMessage_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", listMessage_.c_str());

    ImGui::SeparatorText("Double-click actions");
    bool on = core.setting(ui::kDoubleClickEditKey).value_or("1") != "0";
    if (ImGui::Checkbox("Double-click edits the object (DBLCLKEDIT)", &on))
        core.setSetting(ui::kDoubleClickEditKey, on ? "1" : "0");
    ImGui::TextDisabled(
        "A command line runs with the object selected; PROPERTIES opens the panel. "
        "Empty uses the default.");
    ImGui::BeginDisabled(!on);
    if (ImGui::BeginTable("##dblclick", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (const ui::DoubleClickType& t : ui::doubleClickTypes()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(t.label);
            ImGui::TableNextColumn();
            const std::string skey = std::string("dblclick.") + t.key;
            std::string v = s.getString(skey);
            ImGui::SetNextItemWidth(220);
            if (ImGui::InputTextWithHint(("##" + skey).c_str(), t.defaultAction, &v))
                s.setString(skey, v);
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
}

// The radial tool wheel: hold the right mouse button in the drawing to show it.
void SettingsDialog::wheel(ui::AppCore& core) {
    auto& s = core.settings();
    ImGui::SeparatorText("Tool wheel (hold right-click)");
    bool on = s.getBool("ui.wheel.enabled", true);
    if (ImGui::Checkbox("Show the tool wheel when right-click is held", &on))
        s.setBool("ui.wheel.enabled", on);
    ImGui::SameLine();
    ImGui::TextDisabled("(a short click still opens the menu)");
    int delay = static_cast<int>(s.getNumber("ui.wheel.delayMs", ui::kWheelDefaultDelayMs));
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderInt("Hold time (ms)", &delay, 100, 800))
        s.setNumber("ui.wheel.delayMs", delay);

    static const char* const kChoices[] = {
        "LINE",  "POLYLINE", "CIRCLE", "ARC",    "RECTANGLE", "HATCH", "TEXT",   "MOVE",
        "COPY",  "ROTATE",   "SCALE",  "MIRROR", "STRETCH",   "TRIM",  "OFFSET", "FILLET",
        "ERASE", "EXTEND",   "DIM",    "UNDO",   "REDO",      "ZOOM",  "LAYER",  "ORTHO"};
    std::vector<std::string> tools = ui::parseWheelTools(s.getString("ui.wheel.tools", ""));
    bool changed = false;
    // The eight slots are folded away: most people keep the defaults, and they filled the page.
    if (!ImGui::CollapsingHeader("Choose the tools on the wheel")) {
        std::string line;
        for (const std::string& t : tools)
            line += (line.empty() ? "" : ", ") + (t.empty() ? std::string("-") : t);
        ImGui::TextDisabled("%s", line.c_str());
        return;
    }
    ImGui::TextDisabled(
        "Slots run clockwise from the top. Pick a tool, or type any command or alias.");
    for (int i = 0; i < ui::kWheelSlots; ++i) {
        std::string& t = tools[static_cast<std::size_t>(i)];
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Slot %d", i + 1);
        ImGui::SameLine(90);
        ImGui::SetNextItemWidth(180);
        if (ImGui::InputText("##tool", &t)) {
            t = cmd::toUpperAscii(cmd::trim(t));
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::BeginCombo("##pick", "Choose...", ImGuiComboFlags_WidthFitPreview)) {
            for (const char* c : kChoices)
                if (ImGui::Selectable(c)) {
                    t = c;
                    changed = true;
                }
            if (ImGui::Selectable("(empty slot)")) {
                t.clear();
                changed = true;
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    if (changed)
        s.setString("ui.wheel.tools", ui::formatWheelTools(tools));
    if (ImGui::Button("Reset wheel"))
        s.setString("ui.wheel.tools", ui::formatWheelTools(ui::defaultWheelTools()));
}

void SettingsDialog::display(ui::AppCore& core) {
    auto& s = core.settings();
    static const char* const kThemes[] = {"Dark", "Light", "Follow Windows"};
    const int mode = hooks_.themeMode ? hooks_.themeMode() : 0;
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Theme", kThemes[std::clamp(mode, 0, 2)])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(kThemes[i], i == mode) && hooks_.setThemeMode)
                hooks_.setThemeMode(i);
        ImGui::EndCombo();
    }
    int percent = static_cast<int>(std::lround(s.getNumber("ui.scale", 1.0) * 100.0));
    ImGui::SetNextItemWidth(260);
    ImGui::SliderInt("Interface scale (%)", &percent, 75, 200);
    if (ImGui::IsItemDeactivatedAfterEdit()) {  // rebuild fonts once, when the slider is released
        s.setNumber("ui.scale", percent / 100.0);
        if (hooks_.scaleChanged)
            hooks_.scaleChanged();
    }
    bool grid = s.getBool("view.grid", false);
    if (ImGui::Checkbox("Show grid", &grid))
        s.setBool("view.grid", grid);
    bool hoverTip = s.getBool("hover.tooltip", true);
    if (ImGui::Checkbox("Tooltip with type, layer and size when resting on an object", &hoverTip))
        s.setBool("hover.tooltip", hoverTip);

    ImGui::SeparatorText("Crosshair");
    bool crossOn = s.getString("view.crosshair", "full") != "off";
    if (ImGui::Checkbox("Show the crosshair", &crossOn)) {
        s.setString("view.crosshair", crossOn ? "full" : "off");
    }
    if (s.getString("view.crosshair", "full") == "short") {  // older setting: 10 %
        s.setString("view.crosshair", "full");
        s.setNumber("view.crosshairSize", 10);
    }
    ImGui::BeginDisabled(!crossOn);
    intSlider(core, "Crosshair size, % (CURSORSIZE)", "view.crosshairSize", 1, 100, 100);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Colours");
    const bool light = hooks_.themeMode && hooks_.themeMode() == 1;
    const ui::Theme theme = light ? ui::lightTheme() : ui::darkTheme();
    hexColorSetting(core, "Model space background", "ui.viewportBackground", theme.bgViewport);
    hexColorSetting(core, "Crosshair", "view.crosshairColor", ui::withAlpha(theme.text, 0.6));
    hexColorSetting(core, "AutoSnap marker", "osnap.markerColor",
                    light ? Rgba{0xC7, 0x77, 0x00, 255} : Rgba{0xF2, 0xA9, 0x3B, 255});

    ImGui::SeparatorText("Fading");
    intSlider(core, "Locked layer fade, % (LAYLOCKFADECTL)", "view.lockFade", 0, 90, 60);
}

void SettingsDialog::drafting(ui::AppCore& core) {
    auto& s = core.settings();
    const drafting::DraftingSettings cur = drafting::DraftingSettings::read(core);
    auto toggle = [&](const char* label, const char* key, bool fallback = false) {
        bool on = s.getBool(key, fallback);
        if (ImGui::Checkbox(label, &on)) {
            core.setSetting(key, on ? "1" : "0");
            // Ortho and polar tracking exclude each other.
            if (on && std::string(key) == "drafting.ortho")
                s.setBool("drafting.polar", false);
            if (on && std::string(key) == "drafting.polar")
                s.setBool("drafting.ortho", false);
        }
    };
    // A number kept as text in the settings; applied when the field is left and the text parses.
    auto number = [&](const char* label, const char* key, double shown, const char* format) {
        double v = shown;
        ImGui::SetNextItemWidth(140);
        ImGui::InputDouble(label, &v, 0, 0, format, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivatedAfterEdit() && v > 0 && std::isfinite(v)) {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.10g", v);
            core.setSetting(key, buf);
        }
    };
    if (!ImGui::BeginTabBar("##drafttabs"))
        return;
    if (ImGui::BeginTabItem("Snap and Grid")) {
        toggle("Snap on (F9)", drafting::keys::snap);
        number("Snap spacing", drafting::keys::snapSpacing, cur.snapSpacing, "%.6g");
        number("Snap Y spacing", drafting::keys::snapSpacingY, cur.snapSpacingY, "%.6g");
        int snapType = cur.polarSnap ? 1 : 0;
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo("Snap type", &snapType, "Grid snap\0Polar snap\0"))
            core.setSetting(drafting::keys::snapType, snapType == 1 ? "polar" : "grid");
        if (cur.polarSnap)
            number("Polar distance", drafting::keys::polarDistance,
                   cur.polarDistance > 0 ? cur.polarDistance : cur.snapSpacing, "%.6g");
        {
            // SNAPANG and SNAPBASE: the lattice angle and the point it passes through.
            double ang = cur.snapAngleDeg;
            ImGui::SetNextItemWidth(140);
            ImGui::InputDouble("Snap angle", &ang, 0, 0, "%.6g", ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(ang)) {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.10g", ang);
                core.setSetting(drafting::keys::snapAngle, buf);
            }
            double bx[2] = {cur.snapBase.x, cur.snapBase.y};
            ImGui::SetNextItemWidth(220);
            ImGui::InputScalarN("Snap base (X, Y)", ImGuiDataType_Double, bx, 2, nullptr, nullptr,
                                "%.6g", ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::IsItemDeactivatedAfterEdit() && std::isfinite(bx[0]) &&
                std::isfinite(bx[1])) {
                char buf[80];
                std::snprintf(buf, sizeof buf, "%.10g,%.10g", bx[0], bx[1]);
                core.setSetting(drafting::keys::snapBase, buf);
            }
        }
        ImGui::Separator();
        toggle("Grid on (F7)", drafting::keys::grid);
        number("Grid spacing", drafting::keys::gridSpacing, cur.gridSpacing, "%.6g");
        int style = cur.gridLines ? 1 : 0;
        ImGui::SetNextItemWidth(140);
        if (ImGui::Combo("Grid style", &style, "Dots\0Lines\0"))
            core.setSetting(drafting::keys::gridStyle, style == 1 ? "lines" : "dots");
        ImGui::Separator();
        toggle("Dynamic UCS", drafting::keys::dynUcs);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Polar Tracking")) {
        toggle("Polar tracking on (F10)", drafting::keys::polar);
        toggle("Ortho on (F8)", drafting::keys::ortho);
        std::string inc;
        for (double d : cur.polarIncrementsDeg) {
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.10g", d);
            inc += (inc.empty() ? "" : ",") + std::string(buf);
        }
        static std::string text;
        static std::string shownFor;
        if (shownFor != inc) {
            text = inc;
            shownFor = inc;
        }
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("Increment angles (deg)", &text);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (drafting::parsePolarIncrements(text).empty())
                text = inc;  // not valid: put the old value back
            else
                core.setSetting(drafting::keys::polarIncrement, text);
        }
        ImGui::TextDisabled("Angles between 0 and 180, separated by commas: 90 or 30,45.");
        std::string extra = core.setting(drafting::keys::polarAdditional).value_or("");
        static std::string extraText;
        static std::string extraShownFor;
        if (extraShownFor != extra) {
            extraText = extra;
            extraShownFor = extra;
        }
        ImGui::SetNextItemWidth(140);
        ImGui::InputText("Additional angles (deg)", &extraText);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (!drafting::parsePolarAngles(extraText))
                extraText = extra;
            else
                core.setSetting(drafting::keys::polarAdditional, extraText);
        }
        ImGui::TextDisabled("Single angles tracked as they are, not their multiples: 17,127.5.");
        toggle("Measure angles relative to the last segment", drafting::keys::polarRelative);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Object Snap")) {
        toggle("Object snap on (F3)", drafting::keys::osnap, true);
        ImGui::Separator();
        auto mask = cur.modes;
        bool changed = false;
        // Two columns, as in AutoCAD's Object Snap tab.
        if (ImGui::BeginTable("##snapmodes", 2)) {
            for (int i = 0; i < drafting::kSnapModeCount; ++i) {
                ImGui::TableNextColumn();
                const auto mode = static_cast<drafting::SnapMode>(i);
                bool on = (mask & drafting::maskOf(mode)) != 0;
                const std::string label(drafting::snapModeLabel(mode));
                if (ImGui::Checkbox(label.c_str(), &on)) {
                    mask = on ? (mask | drafting::maskOf(mode)) : (mask & ~drafting::maskOf(mode));
                    changed = true;
                }
            }
            ImGui::EndTable();
        }
        if (ImGui::SmallButton("Select all")) {
            mask = drafting::kAllSnapModes;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear all")) {
            mask = 0;
            changed = true;
        }
        if (changed)
            core.setSetting(drafting::keys::osnapModes, drafting::formatSnapModes(mask));
        ImGui::Separator();
        number("Aperture (pixels)", drafting::keys::osnapAperture, cur.aperturePx, "%.0f");
        toggle("Show the aperture box at point prompts (APBOX)", "osnap.apbox");
        ImGui::SeparatorText("AutoSnap");
        toggle("Show the snap marker", "osnap.marker", true);
        intSlider(core, "Marker size (pixels)", "osnap.markerSize", 4, 40, 12);
        ImGui::TextDisabled("The marker colour is on the Display page.");
        ImGui::Separator();
        toggle("Object snap tracking (OTRACK)", drafting::keys::otrack);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Dynamic Input")) {
        toggle("Dynamic input on (F12)", drafting::keys::dynInput, true);
        ImGui::TextDisabled(
            "Distance and angle (or X and Y) next to the cursor; Tab moves "
            "between the fields.");
        toggle("Typed x,y after the first point is absolute", drafting::keys::dynInputAbsolute);
        ImGui::TextDisabled(
            "Off (default): 10,5 is relative to the last point, #10,5 is absolute.");
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Selection")) {
        auto flag = [&](const char* label, const char* key, bool fallback, const char* hint) {
            bool on = core.settings().getBool(key, fallback);
            if (ImGui::Checkbox(label, &on))
                core.setSetting(key, on ? "1" : "0");
            ImGui::TextDisabled("%s", hint);
            ImGui::Spacing();
        };
        flag("Selection cycling (SELCYC)", "ui.selectionCycling", false,
             "Click the same spot again to pick the next object under it. Alt-click does this "
             "whenever the switch is off.");
        {
            const bool pickAdd = core.setting("selection.pickadd").value_or("2") != "0";
            bool on = pickAdd;
            if (ImGui::Checkbox("Picks add to the selection (PICKADD)", &on))
                core.setSetting("selection.pickadd", on ? "2" : "0");
            ImGui::TextDisabled(
                "On: Shift removes. Off: a pick replaces the selection and Shift adds.");
            ImGui::Spacing();
            bool first = core.setting("selection.pickfirst").value_or("1") != "0";
            if (ImGui::Checkbox("Select objects before a command (PICKFIRST)", &first))
                core.setSetting("selection.pickfirst", first ? "1" : "0");
            ImGui::TextDisabled("On: a command uses the objects already selected.");
            ImGui::Spacing();
            bool dragWin = core.setting("selection.pickdrag").value_or("1") != "0";
            if (ImGui::Checkbox("Drag to select with a window (PICKDRAG)", &dragWin))
                core.setSetting("selection.pickdrag", dragWin ? "1" : "0");
            ImGui::TextDisabled("Off: click one corner of the window, then click the other.");
            ImGui::Spacing();
        }
        flag("Quick properties (QPMODE)", "ui.quickProps", false,
             "A small panel next to the cursor shows the common properties of the selection.");
        ImGui::SeparatorText("Pick box and grips");
        intSlider(core, "Pick box size (PICKBOX)", "pick.aperturePx", 1, 50, 6);
        intSlider(core, "Grip size (GRIPSIZE)", "grips.size", 1, 20, 4);
        aciSetting(core, "Grip colour (GRIPCOLOR)", "grips.color", 150);
        aciSetting(core, "Grip under the cursor (GRIPHOVER)", "grips.hover", 11);
        aciSetting(core, "Selected grip (GRIPHOT)", "grips.hot", 12);
        if (ImGui::SmallButton("Default grip colours")) {
            for (const char* k : {"grips.color", "grips.hover", "grips.hot"})
                core.setSetting(k, "");
        }
        ImGui::Spacing();
        {
            bool corner = core.setting("ui.quickProps.location").value_or("cursor") == "corner";
            if (ImGui::Checkbox("Quick properties in the corner of the drawing", &corner))
                core.setSetting("ui.quickProps.location", corner ? "corner" : "cursor");
            ImGui::TextDisabled("Off: next to the pointer where you selected (QPLOCATION 0).");
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void SettingsDialog::files(ui::AppCore& core) {
    auto& s = core.settings();
    bool on = s.getBool("autosave.enabled", true);
    if (ImGui::Checkbox("Autosave unsaved drawings", &on))
        s.setBool("autosave.enabled", on);
    int minutes = static_cast<int>(s.getNumber("autosave.minutes", 10));
    ImGui::BeginDisabled(!on);
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderInt("Every (minutes, SAVETIME)", &minutes, 1, 120))
        s.setNumber("autosave.minutes", minutes);
    ImGui::EndDisabled();
    bool bak = s.getBool("files.backup", true);
    if (ImGui::Checkbox("Keep the previous file as .bak when saving (ISAVEBAK)", &bak))
        s.setBool("files.backup", bak);
    ImGui::Spacing();
    int fmt = s.getString("files.defaultFormat", "dxf") == "dxf" ? 0 : 1;
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Default save format", &fmt, "DXF (.dxf)\0TBCAD (.tbcad, experimental)\0"))
        s.setString("files.defaultFormat", fmt == 0 ? "dxf" : "tbcad");
    ImGui::TextDisabled("A drawing opened from a .dxf file is saved back as DXF.");
    {
        static constexpr const char* kVer[] = {"auto", "r12", "r2000", "r2007"};
        const std::string cur = s.getString("files.dxfVersion", "auto");
        int v = 0;
        for (int i = 0; i < 4; ++i)
            if (cur == kVer[i])
                v = i;
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("DXF version", &v,
                         "Automatic\0R12 (AC1009)\0R2000 (AC1015)\0R2007 (AC1021, UTF-8)\0"))
            s.setString("files.dxfVersion", kVer[v]);
    }
    ImGui::Spacing();
    {
        const std::vector<std::string> names = core.templateNames();
        std::string current = s.getString("template.default", "");
        if (current.empty())
            current = tmpl::kBuiltinTemplate;
        ImGui::SetNextItemWidth(260);
        if (ImGui::BeginCombo("Default template", current.c_str())) {
            for (const std::string& n : names)
                if (ImGui::Selectable(n.c_str(), n == current))
                    s.setString("template.default", n == tmpl::kBuiltinTemplate ? "" : n);
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Templates are .tbt files in %s. Save one with SAVEASTEMPLATE.",
                            core.templateFolder().string().c_str());
    }
    ImGui::Spacing();
    {
        // One folder per line; stored with ";" between them. The drawing's own folder, TBCAD's
        // "plot styles" folder, these, AutoCAD's and Downloads / Documents are searched in order.
        static std::string folders;
        static std::string shown;
        const std::string stored = s.getString("plot.stylepaths", "");
        if (stored != shown) {
            shown = stored;
            folders = stored;
            std::replace(folders.begin(), folders.end(), ';', '\n');
        }
        ImGui::TextUnformatted("Plot style (.ctb / .stb) search folders");
        if (ImGui::InputTextMultiline("##stylepaths", &folders, ImVec2(480, 70))) {
            std::string joined = folders;
            std::replace(joined.begin(), joined.end(), '\n', ';');
            shown = joined;
            s.setString("plot.stylepaths", joined);
        }
        ImGui::TextDisabled(
            "One folder per line. Looked up after the drawing's folder and TBCAD's, before "
            "AutoCAD's, Downloads and Documents.");
    }
    ImGui::Spacing();
    {
        bool check = s.getBool("update.check", true);
        if (ImGui::Checkbox("Look for a new version when TBCAD starts", &check))
            s.setBool("update.check", check);
        static constexpr const char* kChan[] = {"auto", "stable", "test"};
        const std::string cur = s.getString("update.channel", "auto");
        int c = 0;
        for (int i = 0; i < 3; ++i)
            if (cur == kChan[i])
                c = i;
        ImGui::SetNextItemWidth(260);
        if (ImGui::Combo("Updates", &c,
                         "Same kind as this version\0Final releases only\0Test builds too\0"))
            s.setString("update.channel", kChan[c]);
        ImGui::TextDisabled("Help > Check for Updates looks right away.");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Settings folder: %s", core.settingsFolder().string().c_str());
}

void SettingsDialog::aliases(ui::AppCore& core) {
    ui::AliasEditor editor(core.registry(), core.settingsFolder() / "aliases.json");
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##aliasFilter", "Filter...", &aliasFilter_);
    ImGui::SameLine();
    ImGui::TextDisabled("Commands are never tied to AutoCAD names: change any alias here.");

    const float footer = ImGui::GetFrameHeightWithSpacing() * 3 + 8;
    if (ImGui::BeginTable("##aliases", 5,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable,
                          ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Alias", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        std::string toRemove;
        for (const auto& row : editor.rows()) {
            if (!containsNoCase(row.alias, aliasFilter_) &&
                !containsNoCase(row.command, aliasFilter_))
                continue;
            ImGui::PushID(row.alias.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.alias.c_str());
            ImGui::TableNextColumn();
            if (row.command.empty())
                ImGui::TextColored(kWarn, "(none)");
            else
                ImGui::TextUnformatted(row.command.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.source == cmd::AliasEntry::Source::User ? "User"
                                                                               : "Built-in");
            ImGui::TableNextColumn();
            if (row.conflict)
                ImGui::TextColored(kWarn, "! %s", row.note.c_str());
            else if (!row.note.empty())
                ImGui::TextColored(kMuted, "%s", row.note.c_str());
            ImGui::TableNextColumn();
            if (row.source == cmd::AliasEntry::Source::User && ImGui::SmallButton("Remove"))
                toRemove = row.alias;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (!toRemove.empty())
            aliasError_ = editor.remove(toRemove);
    }

    ImGui::SetNextItemWidth(110);
    ImGui::InputTextWithHint("##newAlias", "Alias", &newAlias_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    if (ImGui::BeginCombo("##newCommand",
                          newCommand_.empty() ? "Command..." : newCommand_.c_str())) {
        for (const auto* c : core.registry().list()) {
            if (c->description().empty())
                continue;  // internal commands
            std::string name(c->name());
            if (ImGui::Selectable(name.c_str(), name == newCommand_))
                newCommand_ = name;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add / change")) {
        aliasError_ = editor.set(newAlias_, newCommand_);
        if (aliasError_.empty())
            newAlias_.clear();
    }
    if (!aliasError_.empty())
        ImGui::TextColored(kWarn, "%s", aliasError_.c_str());

    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##pgpPath", "Path of an acad.pgp style file", &pgpPath_);
    ImGui::SameLine();
    if (ImGui::Button("Import##pgp")) {
        std::ifstream in(platform::pathFromUtf8(pgpPath_), std::ios::binary);
        if (!in) {
            pgpMessage_ = "Cannot read that file.";
        } else {
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            ui::AliasImportReport rep = editor.importPgp(text);
            pgpMessage_ =
                !rep.error.empty()
                    ? "Not imported: " + rep.error
                    : "Imported " + std::to_string(rep.added) + " alias(es)" +
                          (rep.skipped.empty() ? std::string(".")
                                               : ", skipped " + std::to_string(rep.skipped.size()) +
                                                     ": " + rep.skipped.front());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Export##pgp")) {
        std::ofstream out(platform::pathFromUtf8(pgpPath_), std::ios::binary | std::ios::trunc);
        out << editor.exportPgp();
        pgpMessage_ = out ? "Aliases written." : "Cannot write that file.";
    }
    if (!pgpMessage_.empty())
        ImGui::TextColored(kMuted, "%s", pgpMessage_.c_str());
}

void SettingsDialog::keyProfile(ui::AppCore& core) {
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##keyFile", "Path of a key binding file (.json)", &keyFile_);
    ImGui::SameLine();
    if (ImGui::Button("Import##keys")) {
        const std::string err = core.importKeybindings(platform::pathFromUtf8(keyFile_));
        keyFileMessage_ = err.empty() ? "Key bindings imported." : err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Export##keys")) {
        const std::string err = core.exportKeybindings(platform::pathFromUtf8(keyFile_));
        keyFileMessage_ = err.empty() ? "Key bindings written." : err;
    }
    if (!keyFileMessage_.empty())
        ImGui::TextColored(kMuted, "%s", keyFileMessage_.c_str());
}

void SettingsDialog::captureKey(ui::AppCore& core) {
    // The next non-modifier key press (with the held modifiers) becomes the chord. Esc cancels.
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (!ImGui::IsKeyPressed(key, false))
            continue;
        if (key == ImGuiKey_Escape) {
            capturing_.clear();
            keyMessage_.clear();
            return;
        }
        auto name = keyNameFor(key);
        if (!name)
            continue;  // modifiers and keys we cannot bind
        platform::KeyChord chord;
        chord.key = *name;
        const ImGuiIO& io = ImGui::GetIO();
        chord.ctrl = io.KeyCtrl;
        chord.shift = io.KeyShift;
        chord.alt = io.KeyAlt;
        ui::KeybindingEditor editor(core.keybindings(), core.settingsFolder() / "keybindings.json");
        auto out = editor.set(capturing_, chord.toString());
        if (!out.conflictWith.empty()) {
            pendingChord_ = chord.toString();
            pendingConflict_ = out.conflictWith;
            keyMessage_.clear();
        } else {
            keyMessage_ = out.ok ? std::string{} : out.error;
            capturing_.clear();
        }
        return;
    }
}

void SettingsDialog::keybindings(ui::AppCore& core) {
    ui::KeybindingEditor editor(core.keybindings(), core.settingsFolder() / "keybindings.json");
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##keyFilter", "Filter...", &keyFilter_);
    ImGui::SameLine();
    ImGui::TextDisabled("Click Change, then press the new keys. Esc cancels.");

    if (!capturing_.empty() && pendingConflict_.empty())
        captureKey(core);

    const float footer = ImGui::GetFrameHeightWithSpacing() * 3 + 8;
    if (ImGui::BeginTable("##keys", 5,
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable,
                          ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("##change", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableHeadersRow();
        for (const auto& row : editor.rows()) {
            const std::string label =
                hooks_.actionLabel ? hooks_.actionLabel(row.action) : row.action;
            if (!containsNoCase(label, keyFilter_) && !containsNoCase(row.chord, keyFilter_))
                continue;
            ImGui::PushID(row.action.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label.c_str());
            ImGui::TableNextColumn();
            if (capturing_ == row.action)
                ImGui::TextColored(kWarn, "Press keys...");
            else if (row.chord.empty())
                ImGui::TextColored(kMuted, "(unbound)");
            else if (!row.conflictWith.empty())
                ImGui::TextColored(kWarn, "%s  ! also %s", row.chord.c_str(),
                                   row.conflictWith.c_str());
            else
                ImGui::TextUnformatted(row.chord.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(kMuted, "%s",
                               row.defaultChord.empty() ? "(none)" : row.defaultChord.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Change")) {
                capturing_ = row.action;
                pendingConflict_.clear();
                keyMessage_.clear();
            }
            ImGui::TableNextColumn();
            if (!row.chord.empty() && ImGui::SmallButton("Unbind"))
                keyMessage_ = editor.set(row.action, "").error;
            ImGui::SameLine();
            if (platform::Keybindings::isCommandAction(row.action)) {
                if (ImGui::SmallButton("Delete"))
                    keyMessage_ = editor.removeCommand(row.action).error;
            } else if (row.overridden && ImGui::SmallButton("Default"))
                keyMessage_ = editor.reset(row.action).error;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (!pendingConflict_.empty()) {
        const std::string other =
            hooks_.actionLabel ? hooks_.actionLabel(pendingConflict_) : pendingConflict_;
        ImGui::TextColored(kWarn, "%s is already used by %s.", pendingChord_.c_str(),
                           other.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Take over")) {
            keyMessage_ = editor.set(capturing_, pendingChord_, true).error;
            capturing_.clear();
            pendingConflict_.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            capturing_.clear();
            pendingConflict_.clear();
        }
    } else if (!keyMessage_.empty()) {
        ImGui::TextColored(kWarn, "%s", keyMessage_.c_str());
    }
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##newCmd", "Command, e.g. LAYER", &keyCommand_);
    ImGui::SameLine();
    if (ImGui::Button("Add command shortcut")) {
        auto out = editor.addCommand(keyCommand_, "");
        keyMessage_ = out.error;
        if (out.ok) {
            capturing_ = std::string(platform::Keybindings::kCommandPrefix) + keyCommand_;
            pendingConflict_.clear();
            keyCommand_.clear();
        }
    }
    keyProfile(core);
}

}  // namespace tbcad::app
