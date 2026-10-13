#pragma once

#include "tbcad/cmd/registry.h"

namespace tbcad::constraintscmd {

// GEOMCON, DIMCON, SHOWCON, DELCON and SOLVECON, plus the commit hook that keeps constrained
// geometry in step when a command edits it. Called by the generated registerAllCommands.
void registerCommands(cmd::Registry& registry);

}  // namespace tbcad::constraintscmd
