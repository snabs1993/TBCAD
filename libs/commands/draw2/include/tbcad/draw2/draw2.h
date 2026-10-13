#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::draw2 {

// POINT [PO], PTYPE, DONUT [DO], REVCLOUD [REVC], CENTERMARK [CM], CENTERLINE [CL] and
// BOUNDARY [BO]. Called by the generated tbcad::registerAllCommands (see module.cmake).
void registerCommands(cmd::Registry& registry);
// Menu entries (File/Draw/Modify) for this module's commands.
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::draw2
