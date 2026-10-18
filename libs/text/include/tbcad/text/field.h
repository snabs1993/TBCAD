#pragma once

#include <string>
#include <string_view>

#include "tbcad/core/core.h"

namespace tbcad::text {

// Fields: parts of a text that are worked out each time it is drawn, written as %<name>% or
// %<name:argument:argument>%.
//   date            today, as YYYY-MM-DD; date:dd/MM/yyyy picks the form (yyyy, BBBB = Buddhist
//                   year, MM, dd, HH, mm)
//   filename        the drawing's file name without folder; filepath: with folder
//   sheet           the sheet number kept in the drawing (setting "sheet.number")
//   var:NAME        any setting kept in the drawing (Header::custom)
//   length:#ID      length of a line, arc, circle, ellipse or polyline (perimeter:#ID is the same);
//                   area:#ID of a circle, full ellipse, closed polyline or hatch (islands taken
//                   away); text:#ID the text of another text object. A last argument gives the
//                   decimals (default 2) and m / cm / mm for the unit the drawing was made in (mm
//                   drawings: length:#12:2:m).
//   radius:#ID, diameter:#ID   of a circle or arc; x:#ID, y:#ID  centre or insertion point
//   layer:#ID       the layer name;  block:#ID  the block name of an insert;
//                   attr:#ID:TAG  an attribute value of an insert
//   scale           the current annotation scale, "1:100"
// A field that cannot be worked out shows as ####.
bool hasFields(std::string_view s);
// `date` is injectable so tests do not depend on the clock: "" = today.
std::string expandFields(const Document& doc, std::string_view s, std::string_view today = {});

}  // namespace tbcad::text
