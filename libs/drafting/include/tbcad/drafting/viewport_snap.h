#pragma once

#include <vector>

#include "tbcad/drafting/osnap.h"
#include "tbcad/layout/layout.h"

namespace tbcad::drafting {

// Where a model point lands on the sheet through a viewport, and back. The view is the viewport's
// centre and scale (1 mm on the sheet = scaleN model units); the middle of its rectangle shows
// `center`.
Vec2 sheetFromModel(const layout::Viewport& vp, Vec2 model);
Vec2 modelFromSheet(const layout::Viewport& vp, Vec2 sheet);

// Snap points of model-space objects seen through the viewports of a layout, for a cursor on the
// sheet. The request is in sheet millimetres; the candidates come back in sheet millimetres too
// (`entity` names the model object). A viewport that is off, does not contain the cursor, or
// whose clip leaves the cursor outside shows nothing; objects on layers frozen in the viewport
// are skipped.
std::vector<SnapCandidate> findViewportSnapCandidates(
    const Document& doc, const index::SpatialIndex& modelIndex, const layout::Layout& layout,
    const SnapRequest& sheetRequest,
    const SnapGeometryRegistry& providers = SnapGeometryRegistry::instance());

}  // namespace tbcad::drafting
