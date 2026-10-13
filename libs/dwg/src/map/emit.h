#pragma once

#include <string>

#include "objects/model.h"

namespace tbcad::dwg::detail {

// Writes the decoded drawing as DXF text (HEADER, TABLES, BLOCKS, ENTITIES) for the DXF importer.
std::string emitDxf(const Drawing& d);

}  // namespace tbcad::dwg::detail
