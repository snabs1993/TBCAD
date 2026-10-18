#include "settings_dialog.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>

#include "key_map.h"
#include "tbcad/cmd/input.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/platform/keybindings.h"
#include "tbcad/platform/paths.h"
#include "tbcad/tmpl/tmpl.h"
#include "tbcad/ui/radial_wheel.h"

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
    if (ImGui::BeginTabBar("##settingsPages")) {
        if (ImGui::BeginTabItem("General", nullptr, pageFlags("General"))) {
            general(core);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display", nullptr, pageFlags("Display"))) {
            display(core);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Drafting", nullptr, pageFlags("Drafting"))) {
            drafting(core);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Files", nullptr, pageFlags("Files"))) {
            files(core);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Aliases", nullptr, pageFlags("Aliases"))) {
            aliases(core);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keybindings", nullptr, pageFlags("Keybindings"))) {
            keybindings(core);
            ImGui::EndTabItem();
        }
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
    int aperture = static_cast<int>(s.getNumber("pick.aperturePx", 6));
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderInt("Pick box (pixels)", &aperture, 3, 20))
        s.setNumber("pick.aperturePx", aperture);
    bool lw = s.getBool("view.lineweights", true);
    if (ImGui::Checkbox("Show line weights", &lw))
        s.setBool("view.lineweights", lw);
    wheel(core);
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
    static const char* const kCross[] = {"full", "short", "off"};
    static const char* const kCrossLabel[] = {"Full screen", "Short (5 %)", "Off"};
    const std::string cross = s.getString("view.crosshair", "full");
    int crossIdx = cross == "short" ? 1 : cross == "off" ? 2 : 0;
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Crosshair", kCrossLabel[crossIdx])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(kCrossLabel[i], i == crossIdx))
                s.setString("view.crosshair", kCross[i]);
        ImGui::EndCombo();
    }
    bool hoverTip = s.getBool("hover.tooltip", false);
    if (ImGui::Checkbox("Tooltip with type, layer and size when resting on an object", &hoverTip))
        s.setBool("hover.tooltip", hoverTip);
    bool rightIsEnter = s.getString("ui.rightClick", "menu") == "enter";
    if (ImGui::Checkbox("Right-click in the drawing acts as Enter (no menu)", &rightIsEnter))
        s.setString("ui.rightClick", rightIsEnter ? "enter" : "menu");
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
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Object Snap")) {
        toggle("Object snap on (F3)", drafting::keys::osnap, true);
        ImGui::Separator();
        auto mask = cur.modes;
        bool changed = false;
        for (int i = 0; i < drafting::kSnapModeCount; ++i) {
            const auto mode = static_cast<drafting::SnapMode>(i);
            bool on = (mask & drafting::maskOf(mode)) != 0;
            const std::string label(drafting::snapModeLabel(mode));
            if (ImGui::Checkbox(label.c_str(), &on)) {
                mask = on ? (mask | drafting::maskOf(mode)) : (mask & ~drafting::maskOf(mode));
                changed = true;
            }
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
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Dynamic Input")) {
        toggle("Dynamic input on (F12)", drafting::keys::dynInput, true);
        ImGui::TextDisabled(
            "Distance and angle (or X and Y) next to the cursor; Tab moves "
            "between the fields.");
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
    if (ImGui::SliderInt("Every (minutes)", &minutes, 1, 60))
        s.setNumber("autosave.minutes", minutes);
    ImGui::EndDisabled();
    ImGui::Spacing();
    int fmt = s.getString("files.defaultFormat", "tbcad") == "dxf" ? 1 : 0;
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Default save format", &fmt, "TBCAD (.tbcad)\0DXF (.dxf)\0"))
        s.setString("files.defaultFormat", fmt == 1 ? "dxf" : "tbcad");
    ImGui::TextDisabled("A drawing opened from a .dxf file is saved back as DXF.");
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
            if (row.overridden && ImGui::SmallButton("Default"))
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
    keyProfile(core);
}

}  // namespace tbcad::app
