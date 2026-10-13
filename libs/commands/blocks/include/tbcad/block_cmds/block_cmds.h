#pragma once

#include "tbcad/cmd/registry.h"
#include "tbcad/drafting/snap_geometry.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::block_cmds {

// Called by the generated tbcad::registerAllCommands / registerAllUi (see module.cmake).
// registerCommands also registers the Insert display generator with geom.
void registerCommands(cmd::Registry& registry);
void registerUi(ui::UiRegistry& ui);

// Object snap on block contents: sets the Insert provider on `registry`. registerCommands does
// this for the application's registry (SnapGeometryRegistry::instance()).
void registerSnapProvider(drafting::SnapGeometryRegistry& registry);

}  // namespace tbcad::block_cmds
