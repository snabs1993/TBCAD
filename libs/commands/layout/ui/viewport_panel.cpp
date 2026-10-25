#include <imgui.h>

#include <cstdio>
#include <string>

#include "tbcad/layout/layout.h"
#include "tbcad/layoutcmd/layoutcmd.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::layoutcmd {

namespace {

void exec(ui::PanelContext& ctx, const std::vector<std::string>& lines) {
    if (!ctx.runCommand)
        return;
    if (ctx.cancelCommand)
        ctx.cancelCommand();
    for (const std::string& l : lines)
        ctx.runCommand(l);
}

void drawPanel(ui::PanelContext& ctx) {
    if (!ctx.doc || !ctx.host)
        return;
    const int id = selectedViewport(*ctx.host, *ctx.doc);
    if (!id) {
        ImGui::TextDisabled("Click a viewport border on a sheet to edit it.");
        return;
    }
    auto lay = layout::findLayout(*ctx.doc, currentLayout(*ctx.host, *ctx.doc));
    const layout::Viewport* v = lay ? lay->findViewport(id) : nullptr;
    if (!v)
        return;
    ImGui::Text("Viewport %d", id);
    static char buf[32];
    static int shownFor = 0;
    static double shownScale = 0;
    if (shownFor != id || shownScale != v->scaleN) {
        std::snprintf(buf, sizeof buf, "1:%g", v->scaleN);
        shownFor = id;
        shownScale = v->scaleN;
    }
    ImGui::BeginDisabled(v->locked);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("Scale", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue))
        exec(ctx, {"VPSCALE", buf});
    ImGui::EndDisabled();
    bool locked = v->locked;
    if (ImGui::Checkbox("Locked", &locked))
        exec(ctx, {"VPLOCK"});
}

}  // namespace

void registerViewportPanel(ui::UiRegistry& ui) {
    ui.addPanel("viewport", "Viewport", drawPanel, false);
}

}  // namespace tbcad::layoutcmd
