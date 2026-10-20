// The View Manager (VIEW): the named views of the drawing, Set Current, New (the current display
// or a window), Update Layers, Rename, Delete and the category. Every change runs -VIEW, so it is
// one undo step and shows on the command line.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/views.h"
#include "tbcad/cmd/host.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::sysvars {

namespace {

struct ViewsPanel {
    std::string selected{};
    std::string newName{};
    bool newWindow = false;
    std::string renameTo{};
    std::string category{};
    std::string categoryFor{};
};

void send(ui::PanelContext& pc, const std::vector<std::string>& lines) {
    if (!pc.runCommand)
        return;
    if (pc.cancelCommand)
        pc.cancelCommand();
    for (const std::string& l : lines)
        pc.runCommand(l);
}

void drawViews(ui::PanelContext& pc) {
    if (!pc.doc || !pc.runCommand)
        return;
    static ViewsPanel st;
    const std::vector<NamedView> views = namedViews(*pc.doc);
    const NamedView* sel = nullptr;
    for (const NamedView& v : views)
        if (v.name == st.selected)
            sel = &v;
    if (!sel && !views.empty()) {
        st.selected = views.front().name;
        sel = &views.front();
    }

    ImGui::BeginChild("views-list", ImVec2(180, 0), ImGuiChildFlags_Borders);
    if (views.empty())
        ImGui::TextDisabled("No named views yet.");
    for (const NamedView& v : views) {
        const std::string label = v.category.empty() ? v.name : v.name + "  [" + v.category + "]";
        if (ImGui::Selectable(label.c_str(), v.name == st.selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            st.selected = v.name;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                send(pc, {"-VIEW", "Restore", v.name});
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::BeginDisabled(pc.commandActive);

    ImGui::SeparatorText("New view");
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##viewnew", "Name of the new view", &st.newName);
    if (ImGui::RadioButton("Current display", !st.newWindow))
        st.newWindow = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Define window", st.newWindow))
        st.newWindow = true;
    bool snapshot = !pc.host || pc.host->setting(kViewLayerSnapshotKey).value_or("1") != "0";
    if (ImGui::Checkbox("Save layer snapshot with view", &snapshot) && pc.host)
        pc.host->setSetting(kViewLayerSnapshotKey, snapshot ? "1" : "0");
    ImGui::BeginDisabled(st.newName.empty());
    if (ImGui::Button("New")) {
        send(pc, {"-VIEW", st.newWindow ? "Window" : "Save", st.newName});
        st.selected = st.newName;
        st.newName.clear();
    }
    ImGui::EndDisabled();

    if (sel) {
        const NamedView v = *sel;  // a copy: the buttons change the list
        ImGui::SeparatorText(("View \"" + v.name + "\"").c_str());
        char buf[160];
        std::snprintf(buf, sizeof buf, "Centre %.6g, %.6g", v.center.x, v.center.y);
        ImGui::TextUnformatted(buf);
        if (v.width > 0) {
            std::snprintf(buf, sizeof buf, "Size %.6g x %.6g", v.width, v.height);
            ImGui::TextUnformatted(buf);
        }
        ImGui::TextUnformatted(v.layers ? "Layer snapshot: saved" : "Layer snapshot: none");
        if (ImGui::Button("Set Current"))
            send(pc, {"-VIEW", "Restore", v.name});
        ImGui::SameLine();
        if (ImGui::Button("Update Layers"))
            send(pc, {"-VIEW", "LAyers", v.name});
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
            send(pc, {"-VIEW", "Delete", v.name});
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("##viewrename", "New name", &st.renameTo);
        ImGui::SameLine();
        ImGui::BeginDisabled(st.renameTo.empty());
        if (ImGui::Button("Rename")) {
            send(pc, {"-VIEW", "Rename", v.name, st.renameTo});
            st.selected = st.renameTo;
            st.renameTo.clear();
        }
        ImGui::EndDisabled();
        if (st.categoryFor != v.name) {
            st.categoryFor = v.name;
            st.category = v.category;
        }
        ImGui::SetNextItemWidth(180);
        ImGui::InputTextWithHint("View category", "(none)", &st.category);
        if (ImGui::IsItemDeactivatedAfterEdit() && st.category != v.category)
            send(pc, {"-VIEW", "Category", v.name, st.category});
        ImGui::TextDisabled(
            "On a layout sheet, Set Current puts the view into the selected "
            "viewport.");
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

}  // namespace

void registerViewsPanel(ui::UiRegistry& ui) {
    ui.addPanel("views", "View Manager", drawViews, false);
}

}  // namespace tbcad::sysvars
