#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::draw2 {

// POINT [PO], PTYPE, DONUT [DO], REVCLOUD [REVC], CENTERMARK [CM], CENTERLINE [CL] and
// BOUNDARY [BO]. Called by the generated tbcad::registerAllCommands (see module.cmake).
void registerCommands(cmd::Registry& registry);

}  // namespace tbcad::draw2
