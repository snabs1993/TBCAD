// The UCS dialog (UCSMAN): the named coordinate systems (Set Current, Rename, Delete, details)
// and the settings of the UCS icon and of UCSFOLLOW. Named systems change through UCS command
// lines (one undo step each); the icon settings are program settings.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/ucs.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::draftingcmds {

namespace {

struct UcsPanel {
    std::string selected = "World";
    std::string renameTo{};
};

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void send(ui::PanelContext& pc, const std::vector<std::string>& lines) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

bool flag(ui::PanelContext& pc, const char* key, bool fallback) {
    if (!pc.host)
        return fallback;
    const auto v = pc.host->setting(key);
    return v ? *v != "0" : fallback;
}

void namedTab(ui::PanelContext& pc, UcsPanel& st) {
    const Document& doc = *pc.doc;
    const std::vector<drafting::Ucs> all = drafting::ucsList(doc);
    const drafting::Ucs cur = drafting::currentUcs(&doc, pc.host);
    const std::string curName = cur.isWorld() ? "World" : cur.name;
    bool found = lower(st.selected) == "world";
    for (const auto& u : all)
        found = found || u.name == st.selected;
    if (!found)
        st.selected = "World";

    ImGui::BeginChild("ucs-list", ImVec2(180, 200), ImGuiChildFlags_Borders);
    auto row = [&](const std::string& name) {
        const bool isCur = lower(name) == lower(curName);
        const std::string label = name + (isCur ? "  (current)" : "");
        if (ImGui::Selectable(label.c_str(), name == st.selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            st.selected = name;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                send(pc, name == "World" ? std::vector<std::string>{"UCS", "World"}
                                         : std::vector<std::string>{"UCS", "Restore", name});
        }
    };
    row("World");
    for (const auto& u : all)
        row(u.name);
    if (cur.name.empty() && !cur.isWorld())
        ImGui::TextDisabled("(unnamed current UCS)");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    const std::string sel = st.selected;  // a copy: the buttons rename and delete it
    const bool world = sel == "World";
    ImGui::BeginDisabled(lower(sel) == lower(curName));
    if (ImGui::Button("Set Current"))
        send(pc, world ? std::vector<std::string>{"UCS", "World"}
                       : std::vector<std::string>{"UCS", "Restore", sel});
    ImGui::EndDisabled();
    ImGui::BeginDisabled(world);
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
        send(pc, {"UCS", "Delete", sel});
    ImGui::SetNextItemWidth(150);
    ImGui::InputTextWithHint("##ucsrename", "New name", &st.renameTo);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.renameTo.empty());
    if (ImGui::Button("Rename")) {
        send(pc, {"UCS", "NAmed", "Rename", sel, st.renameTo});
        st.selected = st.renameTo;
        st.renameTo.clear();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SeparatorText("Details");
    drafting::Ucs shown;
    for (const auto& u : all)
        if (u.name == sel)
            shown = u;
    char buf[96];
    std::snprintf(buf, sizeof buf, "Origin  X %.6g  Y %.6g", shown.origin.x, shown.origin.y);
    ImGui::TextUnformatted(buf);
    std::snprintf(buf, sizeof buf, "X axis  %.6g degrees from world X",
                  shown.angle * 180.0 / 3.14159265358979323846);
    ImGui::TextUnformatted(buf);
    if (!cur.isWorld() && cur.name.empty()) {
        static std::string saveAs;
        ImGui::SeparatorText("Current unnamed UCS");
        ImGui::SetNextItemWidth(150);
        ImGui::InputTextWithHint("##ucssave", "Name to save it as", &saveAs);
        ImGui::SameLine();
        ImGui::BeginDisabled(saveAs.empty());
        if (ImGui::Button("Save")) {
            send(pc, {"UCS", "Save", saveAs});
            st.selected = saveAs;
            saveAs.clear();
        }
        ImGui::EndDisabled();
    }
    ImGui::EndGroup();
}

void settingsTab(ui::PanelContext& pc) {
    if (!pc.host)
        return;
    cmd::IHost& host = *pc.host;
    ImGui::SeparatorText("UCS icon settings");
    bool on = flag(pc, drafting::keys::ucsIcon, true);
    if (ImGui::Checkbox("On", &on))
        host.setSetting(drafting::keys::ucsIcon, on ? "1" : "0");
    bool origin = flag(pc, drafting::keys::ucsIconOrigin, true);
    if (ImGui::Checkbox("Display at UCS origin point", &origin))
        host.setSetting(drafting::keys::ucsIconOrigin, origin ? "1" : "0");
    int size = std::atoi(host.setting(drafting::keys::ucsIconSize).value_or("50").c_str());
    if (size < 5 || size > 95)
        size = 50;
    ImGui::SetNextItemWidth(150);
    if (ImGui::SliderInt("Icon size", &size, 5, 95))
        host.setSetting(drafting::keys::ucsIconSize, std::to_string(size));
    const std::string col = host.setting(drafting::keys::ucsIconColor).value_or("Default");
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("Icon colour", col == "Default" ? "Default (red X, green Y)"
                                                          : ("Colour " + col).c_str())) {
        if (ImGui::Selectable("Default (red X, green Y)", col == "Default"))
            host.setSetting(drafting::keys::ucsIconColor, "Default");
        static const std::pair<const char*, const char*> kColours[] = {
            {"1", "Red"},  {"2", "Yellow"},  {"3", "Green"}, {"4", "Cyan"},
            {"5", "Blue"}, {"6", "Magenta"}, {"7", "White"}, {"8", "Grey"}};
        for (const auto& [n, label] : kColours)
            if (ImGui::Selectable(label, col == n))
                host.setSetting(drafting::keys::ucsIconColor, n);
        ImGui::EndCombo();
    }
    ImGui::SeparatorText("UCS settings");
    bool follow = flag(pc, "drafting.ucsfollow", false);
    if (ImGui::Checkbox("Update view to Plan when UCS is changed (UCSFOLLOW)", &follow))
        host.setSetting("drafting.ucsfollow", follow ? "1" : "0");
}

void drawUcsMan(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    static UcsPanel st;
    ImGui::BeginDisabled(pc.commandActive);
    if (ImGui::BeginTabBar("ucsman-tabs")) {
        if (ImGui::BeginTabItem("Named UCSs")) {
            namedTab(pc, st);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            settingsTab(pc);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
}

}  // namespace

void registerUcsPanel(ui::UiRegistry& ui) {
    ui.addPanel("ucsman", "UCS", drawUcsMan, false);
    ui.addMenuItem("Tools/Named UCS...", "UCSMAN");
}

}  // namespace tbcad::draftingcmds
