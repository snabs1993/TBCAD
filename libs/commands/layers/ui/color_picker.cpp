#include "panel_util.h"
#include "tbcad/layers/layer_ops.h"

namespace tbcad::layers::ui_detail {

namespace {

struct PickerState {
    float rgb[3] = {1, 1, 1};
    bool initialised = false;
};

}  // namespace

bool colorPopup(const char* popupId, Color current, const ColorChoices& allowed,
                std::string& chosen) {
    static PickerState st;
    bool picked = false;
    if (!ImGui::BeginPopup(popupId))
        return false;
    if (ImGui::IsWindowAppearing() || !st.initialised) {
        ImVec4 v = current.kind == ColorKind::Index || current.kind == ColorKind::Rgb
                       ? swatch(current)
                       : ImVec4(1, 1, 1, 1);
        st.rgb[0] = v.x;
        st.rgb[1] = v.y;
        st.rgb[2] = v.z;
        st.initialised = true;
    }
    if (allowed.byLayer && ImGui::Button("ByLayer")) {
        chosen = "ByLayer";
        picked = true;
    }
    if (allowed.byBlock) {
        ImGui::SameLine();
        if (ImGui::Button("ByBlock")) {
            chosen = "ByBlock";
            picked = true;
        }
    }
    if (allowed.byParent) {
        ImGui::SameLine();
        if (ImGui::Button("ByParent")) {
            chosen = "ByParent";
            picked = true;
        }
    }
    ImGui::TextDisabled("Palette");
    // 1..7 are the named colours; 8..255 follow in rows of 16.
    constexpr int kPerRow = 16;
    for (int i = 1; i <= 255; ++i) {
        ImGui::PushID(i);
        ImVec4 col = swatch(Color::index(static_cast<std::uint8_t>(i)));
        bool selected = current.kind == ColorKind::Index && current.indexValue() == i;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        if (selected)
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        if (ImGui::ColorButton("##c", col,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha,
                               ImVec2(14, 14))) {
            chosen = std::to_string(i);
            picked = true;
        }
        if (selected) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Index %d", i);
        ImGui::PopID();
        if (i % kPerRow != 0 && i != 255)
            ImGui::SameLine(0, 2);
    }
    ImGui::Separator();
    ImGui::TextDisabled("True colour");
    ImGui::ColorPicker3("##rgb", st.rgb,
                        ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                            ImGuiColorEditFlags_PickerHueBar);
    if (ImGui::Button("Use this colour")) {
        auto q = [](float v) { return static_cast<int>(v * 255.0f + 0.5f); };
        chosen = std::to_string(q(st.rgb[0])) + "," + std::to_string(q(st.rgb[1])) + "," +
                 std::to_string(q(st.rgb[2]));
        picked = true;
    }
    if (picked)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return picked;
}

}  // namespace tbcad::layers::ui_detail
