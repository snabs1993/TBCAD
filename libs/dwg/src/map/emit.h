#pragma once

#include <string>

#include "objects/model.h"
#include "tbcad/dxf/lexer.h"

namespace tbcad::dwg::detail {

// The decoded drawing as DXF groups in memory (HEADER, TABLES, BLOCKS, ENTITIES), for
// dxf::importDxfGroups.
dxf::GroupBuffer emitGroups(const Drawing& d);

// The same as DXF text; the decoded drawing as DXF text (HEADER, TABLES, BLOCKS, ENTITIES) for the
// DXF importer.
std::string emitDxf(const Drawing& d);

}  // namespace tbcad::dwg::detail
