#pragma once

#include <memory>

#include "tbcad/cmd/command.h"
#include "tbcad/cmd/registry.h"

namespace tbcad::layers {

std::unique_ptr<cmd::Command> makeLayerPanelCommand();    // LAYER
std::unique_ptr<cmd::Command> makeLayerCommand();         // -LAYER
std::unique_ptr<cmd::Command> makeLayTransCommand();      // LAYTRANS
std::unique_ptr<cmd::Command> makeActiveOnlyCommand();    // ACTIVEONLY
std::unique_ptr<cmd::Command> makeLayerCurrentCommand();  // LAYERCURRENT
std::unique_ptr<cmd::Command> makeTransparencyCommand();  // TRANSPARENCY
std::unique_ptr<cmd::Command> makePropCommand();          // -PROP
std::unique_ptr<cmd::Command> makeLinetypeCommand();
std::unique_ptr<cmd::Command> makeCurrentCommand();      // -CURRENT      // -LINETYPE
std::unique_ptr<cmd::Command> makeLayerStateCommand();   // -LAYERSTATE
std::unique_ptr<cmd::Command> makeLayerFilterCommand();  // -LAYERFILTER

std::unique_ptr<cmd::Command> makeLayWalkCommand();      // LAYWALK
std::unique_ptr<cmd::Command> makeLayWalkStepCommand();  // -LAYWALK

void registerLayerTools(cmd::Registry& registry);  // LAYOFF, LAYFRZ, LAYISO, LAYMRG ...

}  // namespace tbcad::layers
