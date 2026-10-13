#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::mlinecmd {

// MLINE [ML] and MLSTYLE. Also registers the display generator of MLine with geom and its
// explode hook with the blocks module's ExplodeRegistry.
void registerCommands(cmd::Registry& registry);

// Menu entries for MLINE and MLSTYLE.
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::mlinecmd
