#pragma once

#include <cstddef>

#include "tbcad/cmd/registry.h"
#include "tbcad/core/document.h"

namespace tbcad::dxfexport {

// Registers EXPORTDXF and IMPORTDXF. Called by the generated tbcad::registerAllCommands.
void registerCommands(cmd::Registry& registry);

struct MergeSummary {
    std::size_t entities = 0;
    std::size_t layers = 0;  // layers created (existing ones with the same name are reused)
    std::size_t blocks = 0;  // blocks created
};

// Copies the contents of `src` into the open transaction `tx` of `dst`: tables are matched by name
// (created when missing), blocks whose name is already taken are renamed "<name>_1", "<name>_2" ...
Result<MergeSummary> mergeDocument(const Document& dst, Transaction& tx, const Document& src);

}  // namespace tbcad::dxfexport
