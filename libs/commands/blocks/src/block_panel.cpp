#include <cfloat>
#include <string>

#include "commands.h"
#include "tbcad/block_cmds/block_cmds.h"
#include "tbcad/block_cmds/block_list.h"

#ifdef TBCAD_BLOCK_PANEL_IMGUI
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace tbcad::block_cmds {

#ifdef TBCAD_BLOCK_PANEL_IMGUI

namespace {

// The block list: filter box, then one row per block with the number of objects and uses.
// Double-click (or the Insert button) starts INSERT with that block.
void drawBlockPanel(ui::PanelContext& ctx) {
    if (!ctx.doc) {
        ImGui::TextUnformatted("No drawing open");
        return;
    }
    static std::string filter;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##blockfilter", "Filter blocks", &filter);
    const auto rows = blockRows(*ctx.doc, filter);
    if (rows.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled(filter.empty() ? "No blocks yet. Select objects and type B."
                                           : "No block matches the filter");
        return;
    }
    if (ImGui::BeginTable("blocks", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Used", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        static BlockId selected{};
        for (const BlockRow& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool isSel = selected == r.id;
            if (ImGui::Selectable(
                    r.name.c_str(), isSel,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                selected = r.id;
                if (ImGui::IsMouseDoubleClicked(0) && ctx.runCommand)
                    ctx.runCommand("INSERT " + r.name);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%zu", r.objects);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", r.references);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Double-click a block to insert it");
}

}  // namespace

#endif

void registerBlockPanel(ui::UiRegistry& ui) {
#ifdef TBCAD_BLOCK_PANEL_IMGUI
    ui.addPanel("blocks", "Blocks", drawBlockPanel, false);
#else
    (void)ui;
#endif
}

}  // namespace tbcad::block_cmds
