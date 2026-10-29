// Internal: writing a drawing in the unit it shows (see units.cpp).
#pragma once

#include <memory>

#include "tbcad/core/document.h"
#include "tbcad/core/result.h"

namespace tbcad::dxf::detail {

// The $INSUNITS code of a unit (4 = millimetres).
int insunitsCode(LengthUnit u);

// A copy of `doc` with its lengths in the unit the drawing shows (header().units) instead of
// millimetres; layout sheets stay in millimetres.
Result<std::unique_ptr<Document>> inDrawingUnits(const Document& doc);

}  // namespace tbcad::dxf::detail
