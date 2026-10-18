#pragma once

#include <memory>

#include "tbcad/cmd/cmd.h"

namespace tbcad::sysvars {

void registerCommands(cmd::Registry& registry);
std::unique_ptr<cmd::Command> makeRenameCommand();
std::unique_ptr<cmd::Command> makeSetvarCommand();
std::unique_ptr<cmd::Command> makeViewCommand();
std::unique_ptr<cmd::Command> makeAddSelectedCommand();
std::unique_ptr<cmd::Command> makeHyperlinkCommand();
std::unique_ptr<cmd::Command> makeSpaceTransCommand();
std::unique_ptr<cmd::Command> makeDelayCommand();

}  // namespace tbcad::sysvars
