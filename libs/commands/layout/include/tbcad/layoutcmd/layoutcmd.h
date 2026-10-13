#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::layoutcmd {

// Called by the generated tbcad::registerAllCommands (see module.cmake).
// Commands: LAYOUT, MVIEW, VPSCALE, VPLAYER, TITLEBLOCK.
void registerCommands(cmd::Registry& registry);

// The layout the commands work on when none is named: host setting "layout.current", else the
// first layout. Empty when the drawing has no layout.
std::string currentLayout(const cmd::IHost& host, const Document& doc);

}  // namespace tbcad::layoutcmd
