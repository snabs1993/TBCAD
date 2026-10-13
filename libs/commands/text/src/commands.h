#pragma once

#include <memory>

#include "tbcad/cmd/command.h"

namespace tbcad::ui {
class UiRegistry;
}

namespace tbcad::textcmd {

std::unique_ptr<cmd::Command> makeTextCommand();
std::unique_ptr<cmd::Command> makeMTextCommand();
std::unique_ptr<cmd::Command> makeTextEditCommand();
std::unique_ptr<cmd::Command> makeStyleCommand();
std::unique_ptr<cmd::Command> makeStyleManagerCommand();     // -STYLE
std::unique_ptr<cmd::Command> makeTextStylesPanelCommand();  // TEXTSTYLES
std::unique_ptr<cmd::Command> makeAnnotativeCommand();
std::unique_ptr<cmd::Command> makeAnnoScaleCommand();
std::unique_ptr<cmd::Command> makeScaleListCommand();

void registerEditorPanel(ui::UiRegistry& ui);
void registerStylePanel(ui::UiRegistry& ui);

}  // namespace tbcad::textcmd
