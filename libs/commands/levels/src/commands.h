#pragma once

#include <memory>

#include "tbcad/cmd/command.h"

namespace tbcad::levels {

std::unique_ptr<cmd::Command> makeLevelPanelCommand();   // LEVEL
std::unique_ptr<cmd::Command> makeLevelCommand();        // -LEVEL
std::unique_ptr<cmd::Command> makeChangeLevelCommand();  // CHLEVEL
std::unique_ptr<cmd::Command> makeCopyToLevelCommand();  // COPYTOLEVEL
std::unique_ptr<cmd::Command> makeLevelUpCommand();      // LEVELUP
std::unique_ptr<cmd::Command> makeLevelDownCommand();    // LEVELDOWN

}  // namespace tbcad::levels
