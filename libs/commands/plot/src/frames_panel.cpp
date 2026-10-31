// The Plot Frames panel ("plotframes"): the model space sheets of the drawing in a list with
// check boxes, and named plot sets made from the checked sheets. Every change runs PLOTFRAME or
// FRAMESET, so it is undoable and shows on the command line.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "panel.h"
#include "tbcad/layout/plotframes.h"
#include "tbcad/plot/plot.h"

namespace tbcad::plot {

namespace {

struct FramesState {
    std::set<std::string> checked{};  // frame names
    std::string set{};                // the plot set shown
    std::string newName{};
};

FramesState& state() {
    static FramesState s;
    return s;
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : ",") + n;
    return out;
}

void run(ui::PanelContext& pc, std::initializer_list<std::string> lines) {
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

void draw(ui::PanelContext& pc) {
    if (!pc.doc || !pc.host || !pc.runCommand)
        return;
    FramesState& st = state();
    const auto frames = layout::plotFrames(*pc.doc);
    const auto sets = layout::plotSets(*pc.doc);

    // Forget checks of frames that are gone.
    for (auto it = st.checked.begin(); it != st.checked.end();)
        it = std::any_of(frames.begin(), frames.end(),
                         [&](const layout::PlotFrame& f) { return f.name == *it; })
                 ? std::next(it)
                 : st.checked.erase(it);
    std::vector<std::string> checked;  // in frame order
    for (const auto& f : frames)
        if (st.checked.count(f.name))
            checked.push_back(f.name);

    if (ImGui::Button("New sheet"))
        run(pc, {"PLOTFRAME", "New"});
    ImGui::SameLine();
    if (ImGui::Button("From borders"))
        run(pc, {"PLOTFRAME", "Object"});
    ImGui::SameLine();
    if (ImGui::Button("By window"))
        run(pc, {"PLOTFRAME", "Window"});
    ImGui::SameLine();
    bool shown = pc.host->setting("plot.showFrames").value_or("1") != "0";
    if (ImGui::Checkbox("Show on drawing", &shown))
        pc.host->setSetting("plot.showFrames", shown ? "1" : "0");

    if (frames.empty()) {
        ImGui::TextWrapped(
            "No sheets yet. New sheet places a paper outline at a scale in model space; From "
            "borders makes one sheet for each selected border or title block.");
        return;
    }

    ImGui::Text("%d sheets, %d checked", static_cast<int>(frames.size()),
                static_cast<int>(checked.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton("All"))
        for (const auto& f : frames)
            st.checked.insert(f.name);
    ImGui::SameLine();
    if (ImGui::SmallButton("None"))
        st.checked.clear();

    const float listHeight = std::min(
        320.0f, ImGui::GetFrameHeightWithSpacing() * (static_cast<float>(frames.size()) + 1.5f));
    if (ImGui::BeginTable(
            "frames", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
            ImVec2(0, listHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Sheet", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Paper and scale", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        int n = 0;
        for (const auto& f : frames) {
            ++n;
            ImGui::PushID(n);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = st.checked.count(f.name) > 0;
            if (ImGui::Checkbox("##on", &on)) {
                if (on)
                    st.checked.insert(f.name);
                else
                    st.checked.erase(f.name);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d", n);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(layout::describeFrame(f).c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Zoom")) {
                char a[64], b[64];
                std::snprintf(a, sizeof a, "%.6g,%.6g", f.window.min.x, f.window.min.y);
                std::snprintf(b, sizeof b, "%.6g,%.6g", f.window.max.x, f.window.max.y);
                run(pc, {"ZOOM", "Window", a, b});
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::BeginDisabled(checked.empty());
    if (ImGui::Button("Plot checked"))
        run(pc, {"PLOTFRAME", "Plot", joined(checked), ""});
    ImGui::SameLine();
    if (ImGui::Button("Delete checked")) {
        for (const std::string& name : checked)
            run(pc, {"PLOTFRAME", "Delete", name});
        st.checked.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Plot all"))
        run(pc, {"PLOTFRAME", "All", ""});

    // Plot sets.
    ImGui::SeparatorText("Plot sets");
    if (!st.set.empty() && !layout::findPlotSet(*pc.doc, st.set))
        st.set.clear();
    if (ImGui::BeginCombo("Set", st.set.empty() ? "<choose a set>" : st.set.c_str())) {
        for (const auto& p : sets) {
            const std::string label = p.name + " (" + std::to_string(p.frames.size()) + ")";
            if (ImGui::Selectable(label.c_str(), p.name == st.set)) {
                // Choosing a set checks its sheets, so it can be changed and saved again.
                st.set = p.name;
                st.checked = {p.frames.begin(), p.frames.end()};
            }
        }
        ImGui::EndCombo();
    }
    if (!st.set.empty()) {
        if (ImGui::Button("Plot set"))
            run(pc, {"FRAMESET", "Plot", st.set, ""});
        ImGui::SameLine();
        ImGui::BeginDisabled(checked.empty());
        if (ImGui::Button("Save checked to set"))
            run(pc, {"FRAMESET", "New", st.set, joined(checked)});
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Delete set")) {
            run(pc, {"FRAMESET", "Delete", st.set});
            st.set.clear();
        }
    }
    ImGui::InputTextWithHint("##setname", "Name", &st.newName);
    const bool named = !st.newName.empty() && st.newName.find(',') == std::string::npos;
    ImGui::SameLine();
    ImGui::BeginDisabled(!named || checked.empty());
    if (ImGui::Button("New set from checked")) {
        run(pc, {"FRAMESET", "New", st.newName, joined(checked)});
        st.set = st.newName;
        st.newName.clear();
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!named || st.set.empty());
    ImGui::SameLine();
    if (ImGui::Button("Copy set")) {
        run(pc, {"FRAMESET", "Copy", st.set, st.newName});
        st.set = st.newName;
        st.newName.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename set")) {
        run(pc, {"FRAMESET", "Rename", st.set, st.newName});
        st.set = st.newName;
        st.newName.clear();
    }
    ImGui::EndDisabled();
}

}  // namespace

void registerFramesPanel(ui::UiRegistry& registry) {
    registry.addPanel("plotframes", "Plot Frames", draw, false);
}

}  // namespace tbcad::plot
