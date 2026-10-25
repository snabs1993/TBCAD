#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::layoutcmd {

// Called by the generated tbcad::registerAllCommands (see module.cmake).
// Commands: LAYOUT, MVIEW, VPSCALE, VPLAYER, VPLEVEL, TITLEBLOCK.
void registerCommands(cmd::Registry& registry);

// The layout the commands work on when none is named: host setting "layout.current", else the
// first layout. Empty when the drawing has no layout.
std::string currentLayout(const cmd::IHost& host, const Document& doc);

// Click a viewport's border on the sheet to select it: the viewport commands then use it without
// asking. Returns its number, or 0 when none is selected on the current sheet.
int selectedViewport(const cmd::IHost& host, const Document& doc);

void registerUi(ui::UiRegistry& ui);
// The "Viewport" panel (scale and lock of the selected viewport); empty without the GUI.
void registerViewportPanel(ui::UiRegistry& ui);

}  // namespace tbcad::layoutcmd
