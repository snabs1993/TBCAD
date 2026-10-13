#pragma once

#include <memory>

#include "tbcad/cmd/cmd.h"

namespace tbcad::sysvars {

void registerCommands(cmd::Registry& registry);
std::unique_ptr<cmd::Command> makeRenameCommand();

}  // namespace tbcad::sysvars
