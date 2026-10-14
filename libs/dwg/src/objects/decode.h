#pragma once

#include <string>
#include <vector>

#include "container/r2000.h"
#include "format.h"
#include "objects/model.h"

namespace tbcad::dwg::detail {

// Decodes every object named by the object map into `out`. Objects that cannot be decoded are
// skipped and described in `notes` (one line each, capped).
void decodeObjects(Bytes file, const std::vector<MapEntry>& map,
                   const std::vector<ClassInfo>& classes, Drawing& out,
                   std::vector<std::string>& notes, const Format& fmt = {});

// Diagnostic: the header of the first objects and a histogram of all object types (no content).
void describeObjects(Bytes file, const std::vector<MapEntry>& map, const Format& fmt,
                     std::vector<std::string>& lines);

// Lineweight code of the file (index into the standard list) to DXF 370 (1/100 mm); -1 = by layer,
// -2 = by block, -3 = default.
int lineweightToDxf(int code);

// Which revision of the object reader built this program (changes with every reader fix).
const char* readerRevision();

}  // namespace tbcad::dwg::detail
