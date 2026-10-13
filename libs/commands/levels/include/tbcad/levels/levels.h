#pragma once

#include "tbcad/cmd/registry.h"
#include "tbcad/levels/level_ops.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::levels {

void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::levels
