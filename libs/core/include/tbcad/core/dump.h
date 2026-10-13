#pragma once

#include <string>

#include "tbcad/core/document.h"

namespace tbcad {

// Deterministic text dump of the complete document state (header, tables, entities in id order,
// block membership, draw-order overrides). Two documents with equal dumps are equal for every
// practical purpose; tests use it to check undo/redo and file round trips.
std::string canonicalDump(const Document& doc);

// Dump of a single entity (common properties + payload), used by canonicalDump.
std::string dumpEntity(const Entity& e);

}  // namespace tbcad
