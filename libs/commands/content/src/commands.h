#pragma once

#include <memory>

#include "tbcad/cmd/command.h"
#include "tbcad/ui/ui_registry.h"

namespace tbcad::content_cmds {

std::unique_ptr<cmd::Command> makeContentImportCommand();
std::unique_ptr<cmd::Command> makeContentPanelCommand();
std::unique_ptr<cmd::Command> makeContentCloseCommand();
void registerContentPanel(ui::UiRegistry& ui);  // app builds only

}  // namespace tbcad::content_cmds
