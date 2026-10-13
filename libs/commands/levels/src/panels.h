#pragma once

#include "tbcad/ui/ui_registry.h"

namespace tbcad::levels {

// The level panel; defined in ui/levels_panel.cpp when the build has Dear ImGui, else in
// panels_stub.cpp.
void drawLevelsPanel(ui::PanelContext& ctx);

}  // namespace tbcad::levels
