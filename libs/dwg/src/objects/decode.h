#pragma once

#include <string>
#include <vector>

#include "container/r2000.h"
#include "objects/model.h"

namespace tbcad::dwg::detail {

// Decodes every object named by the object map into `out`. Objects that cannot be decoded are
// skipped and described in `notes` (one line each, capped).
void decodeObjects(Bytes file, const std::vector<MapEntry>& map,
                   const std::vector<ClassInfo>& classes, Drawing& out,
                   std::vector<std::string>& notes);

// Lineweight code of the file (index into the standard list) to DXF 370 (1/100 mm); -1 = by layer,
// -2 = by block, -3 = default.
int lineweightToDxf(int code);

}  // namespace tbcad::dwg::detail
