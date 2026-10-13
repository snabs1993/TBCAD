#pragma once

#include <string>

#include "tbcad/select/select.h"

namespace tbcad::select::detail {

std::unique_ptr<cmd::Command> makePurge();
std::unique_ptr<cmd::Command> makeQSelect();
std::unique_ptr<cmd::Command> makeSelectSimilar();
std::unique_ptr<cmd::Command> makeIsolate();
std::unique_ptr<cmd::Command> makeQuickProperties();
std::unique_ptr<cmd::Command> makeChProp();
std::unique_ptr<cmd::Command> makeFilter();
std::unique_ptr<cmd::Command> makeHide();
std::unique_ptr<cmd::Command> makeUnisolate();

// "BYLAYER", "BYBLOCK", an index 1..255 or "r,g,b"; nullopt when it is none of those.
std::optional<Color> parseColor(const std::string& text);
// A width in mm (0..2.11), BYLAYER, BYBLOCK or DEFAULT.
std::optional<LineWeight> parseWeight(const std::string& text);

// What is drawn for an object once ByLayer is looked up.
Color resolvedColor(const Document& doc, const Entity& e);
LinetypeId resolvedLinetype(const Document& doc, const Entity& e);
LineWeight resolvedLineweight(const Document& doc, const Entity& e);
std::string kindName(const Entity& e);

// Model-space objects that are shown: visible, on a layer that is on and thawed.
std::vector<const Entity*> shownEntities(const Document& doc);

}  // namespace tbcad::select::detail
