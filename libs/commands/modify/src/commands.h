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
std::unique_ptr<cmd::Command> makeOops();
std::unique_ptr<cmd::Command> makeLengthen();
std::unique_ptr<cmd::Command> makeReverse();
std::unique_ptr<cmd::Command> makeDivide();
std::unique_ptr<cmd::Command> makeMeasure();
std::unique_ptr<cmd::Command> makeAlign();
std::unique_ptr<cmd::Command> makeMatchProp();
std::unique_ptr<cmd::Command> makeOverkill();
std::unique_ptr<cmd::Command> makePedit();
std::unique_ptr<cmd::Command> makeBlend();
std::unique_ptr<cmd::Command> makeGroup();
std::unique_ptr<cmd::Command> makeUngroup();
std::unique_ptr<cmd::Command> makeGroupEdit();
std::unique_ptr<cmd::Command> makeDrawOrder();
std::unique_ptr<cmd::Command> makeTextToFront();
std::unique_ptr<cmd::Command> makeHatchToBack();
std::unique_ptr<cmd::Command> makeJustifyText();
std::unique_ptr<cmd::Command> makeSplineEdit();
std::unique_ptr<cmd::Command> makeExtrim();
std::unique_ptr<cmd::Command> makeScaleText();
std::unique_ptr<cmd::Command> makeSetByLayer();
std::unique_ptr<cmd::Command> makeFind();
std::unique_ptr<cmd::Command> makeCopyToLayer();

}  // namespace tbcad::modifycmd
