#include "settings_dialog.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>

#include "key_map.h"
#include "tbcad/platform/keybindings.h"

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
    open_ = ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoSavedSettings);
    if (!open_)
        return;
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
    bool rightIsEnter = s.getString("ui.rightClick", "menu") == "enter";
    if (ImGui::Checkbox("Right-click in the drawing acts as Enter (no menu)", &rightIsEnter))
        s.setString("ui.rightClick", rightIsEnter ? "enter" : "menu");
}

void SettingsDialog::drafting(ui::AppCore& core) {
    auto& s = core.settings();
    auto toggle = [&](const char* label, const char* key) {
        bool on = s.getBool(key, false);
        if (ImGui::Checkbox(label, &on)) {
            core.setSetting(key, on ? "1" : "0");
            // Ortho and polar tracking exclude each other.
            if (on && std::string(key) == "drafting.ortho")
                s.setBool("drafting.polar", false);
            if (on && std::string(key) == "drafting.polar")
                s.setBool("drafting.ortho", false);
        }
    };
    toggle("Object snap (OSNAP)", "drafting.osnap");
    toggle("Grid snap (SNAP)", "drafting.snap");
    toggle("Ortho (ORTHO)", "drafting.ortho");
    toggle("Polar tracking (POLAR)", "drafting.polar");
    ImGui::TextDisabled(
        "Snap spacing, polar increment and snap modes: use the SNAP, POLAR and "
        "OSNAP commands.");
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
    ImGui::TextDisabled("Settings folder: %s", core.settingsFolder().string().c_str());
}

void SettingsDialog::aliases(ui::AppCore& core) {
    ui::AliasEditor editor(core.registry(), core.settingsFolder() / "aliases.json");
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##aliasFilter", "Filter...", &aliasFilter_);
    ImGui::SameLine();
    ImGui::TextDisabled("Commands are never tied to AutoCAD names: change any alias here.");

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + 8;
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

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + 8;
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
}

}  // namespace tbcad::app
