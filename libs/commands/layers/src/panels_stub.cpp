#include "panels.h"

namespace tbcad::layers {

// Headless builds have no Dear ImGui; the panels draw nothing.
void drawLayersPanel(ui::PanelContext&) {}
void drawLayWalkPanel(ui::PanelContext&) {}
void drawPropertiesPanel(ui::PanelContext&) {}
void drawQuickProperties(ui::PanelContext&) {}

}  // namespace tbcad::layers
