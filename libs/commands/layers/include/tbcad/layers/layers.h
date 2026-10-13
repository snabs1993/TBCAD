#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::layers {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
// Commands: LAYER (panel), -LAYER, LAYERCURRENT, -PROP, -LINETYPE.
void registerCommands(cmd::Registry& registry);
// Panels "layers" and "properties" (empty stubs when the build has no Dear ImGui).
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::layers
