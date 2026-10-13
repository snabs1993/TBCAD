#pragma once

#include <iosfwd>

#include "tbcad/mcp/protocol.h"

namespace tbcad::mcp {

// Reads newline-delimited JSON-RPC messages from `in` until it ends and writes the replies to
// `out` (one line each, flushed). Only protocol messages go to `out`; diagnostics go to `log`
// (may be null).
void runStdioLoop(std::istream& in, std::ostream& out, McpServerCore& core, std::ostream* log);

}  // namespace tbcad::mcp
