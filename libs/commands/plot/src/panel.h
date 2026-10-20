#pragma once

#include "tbcad/ui/ui_registry.h"

namespace tbcad::plot {

// The Plot panel (settings + live preview). Needs Dear ImGui; compiled into the application build
// only, so headless builds just skip it.
void registerPlotPanel(ui::UiRegistry& registry);
// The plot style table editor ("plotstyle"): all 255 colours with colour, lineweight and screening.
void registerStylePanel(ui::UiRegistry& registry);
// The PUBLISH dialog ("publish"): sheet list, page setup per sheet, one multi-page PDF.
void registerPublishPanel(ui::UiRegistry& registry);

}  // namespace tbcad::plot
