// The Multileader Style Manager (MLEADERSTYLE): the styles, Set current / New / Rename / Delete,
// and the content, arrowhead, landing gap and text settings of the selected one. Every change
// runs -MLEADERSTYLE, so it is one undo step and shows on the command line.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/dim_internal.h"
#include "tbcad/dim/dim.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::dimcmd {

namespace {

struct MLeaderPanel {
    std::string selected = "Standard";
    std::string newName{};
    std::string renameTo{};
};

void send(ui::PanelContext& pc, const std::vector<std::string>& lines) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

void edit(ui::PanelContext& pc, const std::string& style, const char* key,
          const std::string& value) {
    send(pc, {"-MLEADERSTYLE", "Edit", style, key, value, "Done", "Done"});
}

void drawMLeaderStyles(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    static MLeaderPanel st;
    const Document& doc = *pc.doc;
    std::vector<std::string> names = {"Standard"};
    for (const std::string& n : mleaderStyleNames(doc))
        names.push_back(n);
    std::string current = currentMLeaderStyleName(doc);
    if (current.empty())
        current = "Standard";
    if (std::find(names.begin(), names.end(), st.selected) == names.end())
        st.selected = current;

    // A narrow panel (the slim side column) puts the list above the settings, not beside them.
    const bool stack = ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 26.0f;
    ImGui::BeginChild("mls-list",
                      stack ? ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 4.0f) : ImVec2(160, 0),
                      ImGuiChildFlags_Borders);
    for (const std::string& n : names) {
        const std::string label = n + (n == current ? "  (current)" : "");
        if (ImGui::Selectable(label.c_str(), n == st.selected))
            st.selected = n;
    }
    ImGui::EndChild();
    if (!stack)
        ImGui::SameLine();
    ImGui::BeginGroup();
    const std::string sel = st.selected;  // a copy: the buttons rename and delete it
    const bool standard = sel == "Standard";
    ImGui::BeginDisabled(pc.commandActive);
    ImGui::BeginDisabled(sel == current);
    if (ImGui::Button("Set current"))
        send(pc, {"-MLEADERSTYLE", "Current", sel, "Done"});
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(standard);
    if (ImGui::Button("Delete"))
        send(pc, {"-MLEADERSTYLE", "Delete", sel, "Done"});
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##mlsrename", "New name", &st.renameTo);
    ImGui::SameLine();
    ImGui::BeginDisabled(standard || st.renameTo.empty());
    if (ImGui::Button("Rename")) {
        send(pc, {"-MLEADERSTYLE", "Rename", sel, st.renameTo, "Done"});
        st.selected = st.renameTo;
        st.renameTo.clear();
    }
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##mlsnew", "Name of the new style", &st.newName);
    ImGui::SameLine();
    ImGui::BeginDisabled(st.newName.empty());
    if (ImGui::Button("New from current")) {
        send(pc, {"-MLEADERSTYLE", "New", st.newName, "Done"});
        st.selected = st.newName;
        st.newName.clear();
    }
    ImGui::EndDisabled();

    if (standard) {
        ImGui::TextWrapped(
            "Standard uses the current dimension style; make a new style to change it.");
        ImGui::EndDisabled();
        ImGui::EndGroup();
        return;
    }
    const DimStyle* d = doc.dimStyleByName(mleaderCompanionName(sel));
    const std::string content = mleaderContent(doc, sel);

    ImGui::SeparatorText("Content");
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("Multileader type", content.c_str())) {
        for (const char* c : {"Text", "Block", "None"})
            if (ImGui::Selectable(c, content == c))
                edit(pc, sel, "Content", c);
        ImGui::EndCombo();
    }
    const MLeaderStyle ms = mleaderStyleNamed(doc, sel);
    if (content == "Block") {
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("Source block", ms.block.empty() ? "(ask)" : ms.block.c_str())) {
            if (ImGui::Selectable("(ask)", ms.block.empty()))
                edit(pc, sel, "Block", "");
            for (const auto& [id, b] : doc.blocks())
                if (!b.name.empty() && b.name[0] != '*' && b.xrefPath.empty() &&
                    ImGui::Selectable(b.name.c_str(), b.name == ms.block))
                    edit(pc, sel, "Block", b.name);
            ImGui::EndCombo();
        }
    }
    bool anno = ms.annotative;
    if (ImGui::Checkbox("Annotative", &anno))
        edit(pc, sel, "Annotative", anno ? "Yes" : "No");
    if (d) {
        const DimStyle s = *d;
        const TextStyle* ts = s.textStyle.value ? doc.textStyle(s.textStyle) : nullptr;
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("Text style", ts ? ts->name.c_str() : "(current)")) {
            if (ImGui::Selectable("(current)", ts == nullptr))
                edit(pc, sel, "TextStyle", "Current");
            for (const auto& [id, t] : doc.textStyles())
                if (ImGui::Selectable(t.name.c_str(), ts && ts->id == id))
                    edit(pc, sel, "TextStyle", t.name);
            ImGui::EndCombo();
        }
        double th = s.textHeight;
        ImGui::SetNextItemWidth(160);
        ImGui::InputDouble("Text height", &th, 0, 0, "%.6g");
        if (ImGui::IsItemDeactivatedAfterEdit() && th != s.textHeight && th > 0)
            edit(pc, sel, "TextHeight", num(th));

        ImGui::SeparatorText("Leader format");
        static const std::pair<const char*, const char*> kArrows[] = {
            {"Closed filled", "Filled"},
            {"Open", "Open"},
            {"Architectural tick", "Tick"},
            {"Dot", "Dot"},
            {"None", "None"}};
        const int a = static_cast<int>(s.arrowType);
        ImGui::SetNextItemWidth(160);
        if (ImGui::BeginCombo("Arrowhead", kArrows[a].first)) {
            for (int i = 0; i < 5; ++i)
                if (ImGui::Selectable(kArrows[i].first, i == a))
                    edit(pc, sel, "Arrow", kArrows[i].second);
            ImGui::EndCombo();
        }
        double as = s.arrowSize;
        ImGui::SetNextItemWidth(160);
        ImGui::InputDouble("Arrow size", &as, 0, 0, "%.6g");
        if (ImGui::IsItemDeactivatedAfterEdit() && as != s.arrowSize && as > 0)
            edit(pc, sel, "ArrowSize", num(as));
        double gap = s.textGap;
        ImGui::SetNextItemWidth(160);
        ImGui::InputDouble("Landing gap", &gap, 0, 0, "%.6g");
        if (ImGui::IsItemDeactivatedAfterEdit() && gap != s.textGap && gap >= 0)
            edit(pc, sel, "Gap", num(gap));
    }
    int pts = static_cast<int>(ms.maxPoints);
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt("Maximum leader points", &pts);
    if (ImGui::IsItemDeactivatedAfterEdit() && pts != ms.maxPoints && pts >= 2 && pts <= 100)
        edit(pc, sel, "MaxPoints", std::to_string(pts));
    bool dogleg = ms.dogleg;
    if (ImGui::Checkbox("Automatically include landing", &dogleg))
        edit(pc, sel, "Dogleg", dogleg ? "Yes" : "No");
    ImGui::BeginDisabled(!ms.dogleg);
    double len = ms.doglegLength;
    ImGui::SetNextItemWidth(160);
    ImGui::InputDouble("Landing distance (0 = arrow size)", &len, 0, 0, "%.6g");
    if (ImGui::IsItemDeactivatedAfterEdit() && len != ms.doglegLength && len >= 0)
        edit(pc, sel, "DoglegLength", num(len));
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

}  // namespace

void registerMLeaderStylePanel(ui::UiRegistry& ui) {
    ui.addPanel("mleaderstyles", "Multileader Style Manager", drawMLeaderStyles, false);
    ui.addMenuItem("Format/Multileader Style...", "MLEADERSTYLE", "format.mleaderstyle");
}

}  // namespace tbcad::dimcmd
