#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::draw {

// LINE [L], POLYLINE [PL], CIRCLE [C], ARC [A], RECTANGLE [REC], POLYGON [POL], ELLIPSE [EL],
// SPLINE [SPL]. Called by the generated tbcad::registerAllCommands (see module.cmake).
void registerCommands(cmd::Registry& registry);

}  // namespace tbcad::draw
