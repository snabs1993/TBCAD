#pragma once

#include <memory>

#include "tbcad/cmd/command.h"

namespace tbcad::layers {

std::unique_ptr<cmd::Command> makeLayerPanelCommand();    // LAYER
std::unique_ptr<cmd::Command> makeLayerCommand();         // -LAYER
std::unique_ptr<cmd::Command> makeLayerCurrentCommand();  // LAYERCURRENT
std::unique_ptr<cmd::Command> makePropCommand();          // -PROP
std::unique_ptr<cmd::Command> makeLinetypeCommand();      // -LINETYPE

}  // namespace tbcad::layers
