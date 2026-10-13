#pragma once

#include "tbcad/ui/ui_registry.h"

namespace tbcad::plot {

// The Plot panel (settings + live preview). Needs Dear ImGui; compiled into the application build
// only, so headless builds just skip it.
void registerPlotPanel(ui::UiRegistry& registry);

}  // namespace tbcad::plot
