#pragma once

#include <memory>

#include "tbcad/core/core.h"

namespace tbcad::samples {

// A two-storey Thai house plan, unit mm: levels (two floors and a roof), sub-layers, walls with
// hatched section, door/window/furniture blocks, Thai room names and annotative dimensions at
// 1:100, and an A3 layout with one viewport per floor and a title block. Deterministic: the same
// document every call.
std::unique_ptr<Document> buildThaiHouse();

}  // namespace tbcad::samples
