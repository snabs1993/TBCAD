#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::example {

// Called by the generated tbcad::registerAllCommands (see module.cmake).
void registerCommands(cmd::Registry& registry);

}  // namespace tbcad::example
