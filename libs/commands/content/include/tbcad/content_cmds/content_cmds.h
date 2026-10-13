#pragma once

#include "tbcad/cmd/registry.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::content_cmds {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::content_cmds
