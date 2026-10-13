#pragma once

#include "tbcad/mcp/tool.h"

namespace tbcad::mcp {

// Registers every drawing tool (see docs/mcp.md). Which ones are listed for a session depends on
// what the session offers (viewport, export commands).
void registerDrawingTools(ToolSet& tools);

// Prefix of the undo label of every step an AI tool call creates.
inline constexpr const char* kAiLabelPrefix = "AI: ";

}  // namespace tbcad::mcp
