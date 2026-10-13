#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::dimcmd {

// DIMLINEAR [DLI], DIMALIGNED [DAL], DIMRADIUS [DRA], DIMDIAMETER [DDI], DIMANGULAR [DAN],
// LEADER [LE] and DIMSTYLE [D]. Also registers the display generators of Dimension and Leader
// with geom and their explode hooks with the blocks module's ExplodeRegistry.
void registerCommands(cmd::Registry& registry);

// Dimension style panel and menu items. Defined only in application builds (TBCAD_BUILD_APP).
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::dimcmd
