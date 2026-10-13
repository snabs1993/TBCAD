#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::transform {

// MOVE, COPY, ROTATE, SCALE, MIRROR and the hidden GRIPEDIT. Called by the generated
// tbcad::registerAllCommands (see module.cmake).
void registerCommands(cmd::Registry& registry);
// Grip overlay and the "Modify" menu entries.
void registerUi(ui::UiRegistry& registry);

}  // namespace tbcad::transform
