#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cstdio>
#include <string>

#include "panels.h"
#include "tbcad/levels/level_ops.h"

namespace tbcad::levels {

namespace {

// Like the layer panel, edits run commands so each one is an undo step and shows in the log.
void exec(ui::PanelContext& ctx, const std::vector<std::string>& lines) {
    if (!ctx.runCommand)
        return;
    for (const std::string& l : lines)
        ctx.runCommand(l);
    ctx.runCommand("");
}

std::string idRef(LevelId id) {
    return "#" + std::to_string(id.value);
}

struct PanelState {
    LevelId selected{};
    LevelId renaming{};
    bool focusRename = false;
    std::string renameBuf{};
};

PanelState& state() {
    static PanelState s;
    return s;
}

}  // namespace

void drawLevelsPanel(ui::PanelContext& ctx) {
    if (!ctx.doc)
        return;
    const Document& doc = *ctx.doc;
    PanelState& st = state();
    if (!doc.level(st.selected))
        st.selected = doc.header().currentLevel;

    ImGui::BeginDisabled(ctx.commandActive);
    if (ImGui::Button("New Level")) {
        // Next storey: one above the highest level, 3 m taller by default.
        double top = 0;
        bool any = false;
        for (const auto& [id, l] : doc.levels()) {
            top = any ? std::max(top, l.elevation) : l.elevation;
            any = true;
        }
        std::string name = "Level " + std::to_string(doc.levels().size() + 1);
        for (int n = static_cast<int>(doc.levels().size()) + 1; doc.levelByName(name); ++n)
            name = "Level " + std::to_string(n + 1);
        char elev[32];
        std::snprintf(elev, sizeof elev, "%g", top + 3000);
        exec(ctx, {"-LEVEL New", name, elev});
        if (const Level* made = doc.levelByName(name))
            st.selected = made->id;
    }
    ImGui::SameLine();
    const bool canDelete = st.selected != kLevelDefault && doc.level(st.selected);
    ImGui::BeginDisabled(!canDelete);
    if (ImGui::Button("Delete"))
        exec(ctx, {"-LEVEL Delete " + idRef(st.selected)});
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool haveSel = ctx.selection && !ctx.selection->empty() && doc.level(st.selected);
    ImGui::BeginDisabled(!haveSel);
    if (ImGui::Button("Move Selection Here"))
        exec(ctx, {"CHLEVEL", idRef(st.selected)});
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                       ImGuiTableFlags_SizingFixedFit;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + 6;
    if (ImGui::BeginTable("##levels", 6, kFlags, ImVec2(0, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Cur", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Elev (mm)", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Lck", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();
        // Highest level first, like a section drawing.
        auto order = levelOrder(doc);
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const Level* l = doc.level(*it);
            if (!l)
                continue;
            const bool current = l->id == doc.header().currentLevel;
            ImGui::PushID(static_cast<int>(l->id.value));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::RadioButton("##cur", current) && !current)
                exec(ctx, {"-LEVEL Set " + idRef(l->id)});
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Work on this level");
            ImGui::TableSetColumnIndex(1);
            if (st.renaming == l->id) {
                if (st.focusRename) {
                    ImGui::SetKeyboardFocusHere();
                    st.focusRename = false;
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText(
                        "##rename", &st.renameBuf,
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    if (!st.renameBuf.empty() && st.renameBuf != l->name)
                        exec(ctx, {"-LEVEL Rename " + idRef(l->id), st.renameBuf});
                    st.renaming = {};
                } else if (ImGui::IsItemDeactivated()) {
                    st.renaming = {};
                }
            } else {
                if (current)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram));
                if (ImGui::Selectable(l->name.c_str(), st.selected == l->id,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    st.selected = l->id;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        st.renaming = l->id;
                        st.renameBuf = l->name;
                        st.focusRename = true;
                    }
                }
                if (current)
                    ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Click to select, double-click to rename");
            }
            ImGui::TableSetColumnIndex(2);
            {
                double v = l->elevation;
                ImGui::SetNextItemWidth(-1);
                ImGui::InputDouble("##elev", &v, 0, 0, "%.0f");
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%.15g", v);
                    exec(ctx, {"-LEVEL Elevation " + idRef(l->id), buf});
                }
            }
            ImGui::TableSetColumnIndex(3);
            {
                bool v = l->visible;
                ImGui::BeginDisabled(current);
                if (ImGui::Checkbox("##vis", &v))
                    exec(ctx, {std::string("-LEVEL ") + (v ? "Show " : "Hide ") + idRef(l->id)});
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(current ? "The level you work on is always shown"
                                              : "Show or hide this level");
            }
            ImGui::TableSetColumnIndex(4);
            {
                bool v = l->locked;
                if (ImGui::Checkbox("##lck", &v))
                    exec(ctx, {std::string("-LEVEL ") + (v ? "Lock " : "Unlock ") + idRef(l->id)});
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Locked levels show but cannot be edited");
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%zu", entityCount(doc, l->id));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Other levels:");
    ImGui::SameLine();
    const LevelView view = doc.header().levelView;
    static const LevelView kViews[] = {LevelView::DimOthers, LevelView::HideOthers,
                                       LevelView::ShowAll};
    static const char* kLabels[] = {"Dimmed (reference)", "Hidden", "Shown (editable)"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##view", kLabels[static_cast<int>(view)])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(kLabels[i], kViews[i] == view))
                exec(ctx, {"-LEVEL View", levelViewName(kViews[i])});
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Objects on different levels may share the same position.");
}

}  // namespace tbcad::levels
