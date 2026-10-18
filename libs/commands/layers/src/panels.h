#pragma once

#include "tbcad/ui/ui_registry.h"

namespace tbcad::layers {

// The panels; defined in ui/*.cpp when the build has Dear ImGui, else in panels_stub.cpp.
void drawLayersPanel(ui::PanelContext& ctx);
void drawLayWalkPanel(ui::PanelContext& ctx);
void drawPropertiesPanel(ui::PanelContext& ctx);

}  // namespace tbcad::layers
