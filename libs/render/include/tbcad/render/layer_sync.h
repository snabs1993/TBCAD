#pragma once

#include "tbcad/core/document.h"
#include "tbcad/render/scene.h"

namespace tbcad::render {

// Copies layer visibility from the document to the scene using Document::effective(), so
// sub-layers follow their parents (a hidden or frozen parent hides all children). A layer is
// drawn when it is on and not frozen. Call after any layer-table change; display lists carry the
// already resolved colour, lineweight and linetype, so nothing else needs to be synchronised.
void syncLayerVisibility(Scene& scene, const Document& doc);

}  // namespace tbcad::render
