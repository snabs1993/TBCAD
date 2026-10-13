#pragma once

#include <memory>

#include "tbcad/cmd/command.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::palettes {

std::unique_ptr<cmd::Command> makePalettesPanelCommand();
std::unique_ptr<cmd::Command> makePaletteCommand();
void registerPalettePanel(ui::UiRegistry& ui);  // app builds only

}  // namespace tbcad::palettes
