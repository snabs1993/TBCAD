#pragma once

#include <memory>

#include "tbcad/cmd/command.h"

namespace tbcad::draw2::detail {

std::unique_ptr<cmd::Command> makePoint();
std::unique_ptr<cmd::Command> makePointType();
std::unique_ptr<cmd::Command> makeDonut();
std::unique_ptr<cmd::Command> makeRevCloud();
std::unique_ptr<cmd::Command> makeCenterMark();
std::unique_ptr<cmd::Command> makeCenterLine();
std::unique_ptr<cmd::Command> makeBoundary();

}  // namespace tbcad::draw2::detail
