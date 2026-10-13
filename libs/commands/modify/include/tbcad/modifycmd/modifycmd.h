#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::modifycmd {

// Called by the generated tbcad::registerAllCommands (see module.cmake). Adds OFFSET [O],
// TRIM [TR], EXTEND [EX], FILLET [F], CHAMFER [CHA], BREAK [BR], BREAKATPOINT [BAP], JOIN [J]
// and ERASE [E] (the last only when nobody registered it before).
void registerCommands(cmd::Registry& registry);
// Menu entries (File/Draw/Modify) for this module's commands.
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::modifycmd
