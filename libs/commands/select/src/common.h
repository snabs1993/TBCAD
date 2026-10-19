#pragma once

#include <string>

#include "tbcad/select/select.h"

namespace tbcad::select::detail {

std::unique_ptr<cmd::Command> makePurge();
std::unique_ptr<cmd::Command> makeQSelect();
std::unique_ptr<cmd::Command> makeSelectSimilar();
std::unique_ptr<cmd::Command> makeIsolate();
std::unique_ptr<cmd::Command> makeSelectAll();
std::unique_ptr<cmd::Command> makeSelectCommand();
std::unique_ptr<cmd::Command> makeQuickProperties();
std::unique_ptr<cmd::Command> makeChProp();
std::unique_ptr<cmd::Command> makeChange();
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

inline constexpr int kPropCount = 14;
// "Property [Type/Layer/...] <Type>".
std::string propertyPrompt();
// * and ? pattern match; both strings upper case.
bool wildcardMatch(std::string_view pattern, std::string_view text);
// Text or dimension style name of an object; empty when it has none.
std::string styleName(const Document& doc, const Entity& e);
extern const char* const kPropNames[kPropCount];
// Asks Equals/Not (and Greater/Less for numeric properties) and stores it in the query.
cmd::Task askOperator(cmd::CommandContext& ctx, Query& q, std::string_view lead, bool& ok);

// Model-space objects that are shown: visible, on a layer that is on and thawed.
std::vector<const Entity*> shownEntities(const Document& doc);
// The same on every level that is not hidden (other levels shown dimmed count).
std::vector<const Entity*> shownOnAnyLevel(const Document& doc);

}  // namespace tbcad::select::detail
