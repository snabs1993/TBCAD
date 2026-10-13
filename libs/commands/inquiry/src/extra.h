#pragma once

#include <memory>

#include "tbcad/inquiry/inquiry.h"

namespace tbcad::inquiry {

std::unique_ptr<cmd::Command> makeQuickMeasure();
std::unique_ptr<cmd::Command> makeAreaTable();

}  // namespace tbcad::inquiry
