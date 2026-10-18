#pragma once

#include <memory>

#include "tbcad/inquiry/inquiry.h"

namespace tbcad::inquiry {

std::unique_ptr<cmd::Command> makeQuickMeasure();
std::unique_ptr<cmd::Command> makeAreaTable();
std::unique_ptr<cmd::Command> makeAudit();
std::unique_ptr<cmd::Command> makeMassProp();
std::unique_ptr<cmd::Command> makeDwgProps();

}  // namespace tbcad::inquiry
