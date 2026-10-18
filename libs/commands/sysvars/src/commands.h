#pragma once

#include <memory>

#include "tbcad/cmd/cmd.h"

namespace tbcad::sysvars {

void registerCommands(cmd::Registry& registry);
std::unique_ptr<cmd::Command> makeRenameCommand();
std::unique_ptr<cmd::Command> makeSetvarCommand();
// A command named like a drawing variable that asks for its value (see SETVAR).
std::unique_ptr<cmd::Command> makeVarCommand(const std::string& name);
std::unique_ptr<cmd::Command> makeLwDisplayCommand();
std::unique_ptr<cmd::Command> makeViewCommand();
std::unique_ptr<cmd::Command> makeAddSelectedCommand();
std::unique_ptr<cmd::Command> makeHyperlinkCommand();
std::unique_ptr<cmd::Command> makeSpaceTransCommand();
std::unique_ptr<cmd::Command> makeDelayCommand();
// SCRIPT, or RSCRIPT when `repeat`.
std::unique_ptr<cmd::Command> makeScriptCommand(bool repeat);

}  // namespace tbcad::sysvars
