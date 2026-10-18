#pragma once

#include "tbcad/cmd/registry.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::draftingcmds {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);
// Forget the filters registerUi handed to the registry; call before that registry is destroyed when
// the process outlives it (tests). Commands then behave as in a headless run.
void forgetUi();

}  // namespace tbcad::draftingcmds
