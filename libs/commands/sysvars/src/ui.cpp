#include "commands.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::sysvars {

#ifdef TBCAD_SYSVARS_WITH_IMGUI
void registerViewsPanel(ui::UiRegistry& ui);   // ui/views_panel.cpp
void registerVportsPanel(ui::UiRegistry& ui);  // ui/vports_panel.cpp
#endif

void registerUi(ui::UiRegistry& ui) {
    ui.addMenuItem("View/Named Views...", "VIEW");
    ui.addMenuItem("View/Viewports...", "VPORTS");
    ui.addMenuItem("View/Navigation Bar", "NAVBAR");
#ifdef TBCAD_SYSVARS_WITH_IMGUI
    registerViewsPanel(ui);
    registerVportsPanel(ui);
#endif
}

}  // namespace tbcad::sysvars
