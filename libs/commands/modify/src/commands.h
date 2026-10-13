#pragma once

#include <memory>

#include "tbcad/cmd/cmd.h"

namespace tbcad::modifycmd {

std::unique_ptr<cmd::Command> makeOffset();
std::unique_ptr<cmd::Command> makeTrim();
std::unique_ptr<cmd::Command> makeExtend();
std::unique_ptr<cmd::Command> makeFillet();
std::unique_ptr<cmd::Command> makeChamfer();
std::unique_ptr<cmd::Command> makeBreak();
std::unique_ptr<cmd::Command> makeBreakAtPoint();
std::unique_ptr<cmd::Command> makeJoin();
std::unique_ptr<cmd::Command> makeErase();

}  // namespace tbcad::modifycmd
