#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::draw {

// LINE [L], POLYLINE [PL], CIRCLE [C], ARC [A], RECTANGLE [REC], POLYGON [POL], ELLIPSE [EL],
// SPLINE [SPL]. Called by the generated tbcad::registerAllCommands (see module.cmake).
void registerCommands(cmd::Registry& registry);
// Menu entries (File/Draw/Modify) for this module's commands.
void registerUi(ui::UiRegistry& ui);

}  // namespace tbcad::draw
