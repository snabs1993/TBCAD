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
std::unique_ptr<cmd::Command> makeQpModeCommand();
std::unique_ptr<cmd::Command> makeRolloverTipsCommand();
// The Options dialog's variables: PICKBOX, APERTURE, CURSORSIZE, GRIPSIZE, GRIPCOLOR, GRIPHOVER,
// GRIPHOT, LAYLOCKFADECTL, SAVETIME, ISAVEBAK, APBOX, SHORTCUTMENU. Null for any other name.
std::unique_ptr<cmd::Command> makeOptionVarCommand(std::string_view name);
std::unique_ptr<cmd::Command> makeViewCommand();
std::unique_ptr<cmd::Command> makeAddSelectedCommand();
std::unique_ptr<cmd::Command> makeHyperlinkCommand();
std::unique_ptr<cmd::Command> makeSpaceTransCommand();
std::unique_ptr<cmd::Command> makeDelayCommand();
// SCRIPT, or RSCRIPT when `repeat`.
std::unique_ptr<cmd::Command> makeScriptCommand(bool repeat);

}  // namespace tbcad::sysvars
