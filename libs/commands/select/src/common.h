#pragma once

#include <string>

#include "tbcad/select/select.h"

namespace tbcad::select::detail {

std::unique_ptr<cmd::Command> makePurge();
std::unique_ptr<cmd::Command> makeQSelect();
std::unique_ptr<cmd::Command> makeSelectSimilar();
std::unique_ptr<cmd::Command> makeIsolate();
std::unique_ptr<cmd::Command> makeHide();
std::unique_ptr<cmd::Command> makeUnisolate();

// What is drawn for an object once ByLayer is looked up.
Color resolvedColor(const Document& doc, const Entity& e);
LinetypeId resolvedLinetype(const Document& doc, const Entity& e);
LineWeight resolvedLineweight(const Document& doc, const Entity& e);
std::string kindName(const Entity& e);

// Model-space objects that are shown: visible, on a layer that is on and thawed.
std::vector<const Entity*> shownEntities(const Document& doc);

}  // namespace tbcad::select::detail
