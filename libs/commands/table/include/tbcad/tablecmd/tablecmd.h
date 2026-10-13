#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::tablecmd {

// TABLE [TB] and TABLEEDIT [TE]. Also registers the display generator of Table with geom and its
// explode hook with the blocks module's ExplodeRegistry.
void registerCommands(cmd::Registry& registry);

// Menu entries for TABLE and TABLEEDIT.
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::tablecmd
