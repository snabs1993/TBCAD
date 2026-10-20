// The Text Editor contextual tab: shown over the drawing while only text is selected.
#include <imgui.h>

#include <utility>

#include "commands.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::textcmd {

namespace {

void drawContextTab(ui::PanelContext& pc) {
    if (!pc.runCommand)
        return;
    const std::pair<const char*, const char*> tools[] = {
        {"Edit", "TEXTEDIT"},           {"Justify...", "JUSTIFYTEXT"},
        {"Scale...", "SCALETEXT"},      {"Spelling", "SPELL"},
        {"Find and replace", "FIND"},   {"Bring to front", "TEXTTOFRONT"},
        {"Annotative...", "ANNOTATIVE"}};
    bool first = true;
    for (const auto& [label, command] : tools) {
        if (!first)
            ImGui::SameLine();
        first = false;
        if (ImGui::SmallButton(label))
            pc.runCommand(command);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", command);
    }
}

}  // namespace

void registerTextContextTab(ui::UiRegistry& ui) {
    ui.addContextualTab("text", "Text Editor", {EntityKind::Text, EntityKind::MText},
                        drawContextTab);
}

}  // namespace tbcad::textcmd
