#pragma once

#include "tbcad/ui/ui_registry.h"

namespace tbcad::inquiry {

// Drawing Properties ("dwgprops") and Drawing Units ("units"). Need Dear ImGui: compiled into the
// application build only.
void registerPropsPanels(ui::UiRegistry& registry);

}  // namespace tbcad::inquiry
