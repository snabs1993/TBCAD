#pragma once

#include "tbcad/geom/geom.h"

namespace tbcad::geom {

// Appends the text and shapes of the linetype of `host` (DisplayStyle::marks) along its path.
void appendLinetypeMarks(const Document& doc, const DisplayContext& ctx,
                         const DisplayPolyline& host, DisplayList& out);

}  // namespace tbcad::geom
