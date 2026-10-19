#pragma once

#include <memory>

#include "tbcad/cmd/command.h"

namespace tbcad::draw2::detail {

std::unique_ptr<cmd::Command> makePoint();
std::unique_ptr<cmd::Command> makePointType();
std::unique_ptr<cmd::Command> makeDonut();
std::unique_ptr<cmd::Command> makeBreakLine();
std::unique_ptr<cmd::Command> makeRevCloud();
std::unique_ptr<cmd::Command> makeWipeout();
std::unique_ptr<cmd::Command> makeWipeoutFrame();
std::unique_ptr<cmd::Command> makeCenterMark();
std::unique_ptr<cmd::Command> makeCenterLine();
std::unique_ptr<cmd::Command> makeBoundary();
std::unique_ptr<cmd::Command> makeSketch();
std::unique_ptr<cmd::Command> makeRay();
std::unique_ptr<cmd::Command> makeSolid();
std::unique_ptr<cmd::Command> makeXline();
std::unique_ptr<cmd::Command> makeRegion();
std::unique_ptr<cmd::Command> makeUnion();
std::unique_ptr<cmd::Command> makeSubtract();
std::unique_ptr<cmd::Command> makeIntersect();

}  // namespace tbcad::draw2::detail
