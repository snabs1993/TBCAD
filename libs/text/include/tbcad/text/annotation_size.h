#pragma once

#include "tbcad/core/core.h"

namespace tbcad::text {

// The 1-2-5 scale (1, 2, 5, 10, 20, 50 ...) at which annotation drawn with a paper height of
// `paperHeightMm` reads well in this drawing, so new text, leaders and dimensions do not come out
// as specks in a plan of tens of metres. The annotation scale of the drawing wins when it is set;
// otherwise the larger of what the view needs (about 12 screen pixels) and what the size of the
// drawing needs (about extent / 160) decides. `pixelMm` is millimetres per screen pixel (0 when
// there is no view, as in a script); `onSheet` is true on a layout sheet, where millimetres are
// paper millimetres and the answer is always 1.
double suggestedAnnotationScale(const Document& doc, double pixelMm, bool onSheet,
                                double paperHeightMm);

}  // namespace tbcad::text
