#pragma once

#include <memory>

#include "tbcad/cmd/cmd.h"

namespace tbcad::sysvars {

void registerCommands(cmd::Registry& registry);
std::unique_ptr<cmd::Command> makeRenameCommand();
std::unique_ptr<cmd::Command> makeSetvarCommand();
std::unique_ptr<cmd::Command> makeAddSelectedCommand();

}  // namespace tbcad::sysvars
