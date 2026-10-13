#pragma once

#include <memory>

#include "tbcad/cmd/command.h"

namespace tbcad::draw::detail {

std::unique_ptr<cmd::Command> makeLine();
std::unique_ptr<cmd::Command> makePolyline();
std::unique_ptr<cmd::Command> makeCircle();
std::unique_ptr<cmd::Command> makeArc();
std::unique_ptr<cmd::Command> makeRectangle();
std::unique_ptr<cmd::Command> makePolygon();
std::unique_ptr<cmd::Command> makeEllipse();
std::unique_ptr<cmd::Command> makeSpline();

}  // namespace tbcad::draw::detail
