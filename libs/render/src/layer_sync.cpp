#include "tbcad/render/layer_sync.h"

namespace tbcad::render {

void syncLayerVisibility(Scene& scene, const Document& doc) {
    for (const auto& [id, layer] : doc.layers())
        scene.setLayerVisible(id, doc.effective(id).visible());
}

}  // namespace tbcad::render
