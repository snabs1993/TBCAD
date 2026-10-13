#include "panels.h"

namespace tbcad::levels {

// Headless builds have no Dear ImGui; the panel draws nothing.
void drawLevelsPanel(ui::PanelContext&) {}

}  // namespace tbcad::levels
