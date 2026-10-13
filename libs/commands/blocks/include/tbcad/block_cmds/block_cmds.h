#pragma once

#include "tbcad/cmd/registry.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::block_cmds {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
// registerCommands also registers the Insert display generator with geom.
void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::block_cmds
