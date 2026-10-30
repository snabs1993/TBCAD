#include <doctest/doctest.h>

#include "tbcad/core/core.h"
#include "tbcad/render/colors.h"
#include "tbcad/render/layer_sync.h"

using namespace tbcad;
using namespace tbcad::render;

TEST_CASE("syncLayerVisibility follows on/frozen and parent inheritance") {
    Document doc;
    LayerId walls, doors, edge, frozen;
    {
        Transaction tx = doc.begin("layers");
        walls = tx.addLayer(Layer{.name = "Walls"}).value();
        doors = tx.addLayer(Layer{.name = "Doors", .on = false}).value();
        edge = tx.addLayer(Layer{.name = "edge", .parent = doors}).value();
        frozen = tx.addLayer(Layer{.name = "Frozen", .frozen = true}).value();
        tx.commit();
    }
    Scene scene;
    syncLayerVisibility(scene, doc);
    CHECK(scene.layerVisible(kLayerZero));
    CHECK(scene.layerVisible(walls));
    CHECK_FALSE(scene.layerVisible(doors));
    CHECK_FALSE(scene.layerVisible(edge));  // child of a hidden layer
    CHECK_FALSE(scene.layerVisible(frozen));

    {
        Transaction tx = doc.begin("show doors");
        REQUIRE(tx.modifyLayer(doors, [](Layer& l) { l.on = true; }));
        tx.commit();
    }
    syncLayerVisibility(scene, doc);
    CHECK(scene.layerVisible(doors));
    CHECK(scene.layerVisible(edge));
}

TEST_CASE("white and black swap against light and dark backgrounds") {
    const Rgba white{255, 255, 255, 255}, black{0, 0, 0, 255}, red{255, 0, 0, 255};
    const Rgba lightBg{240, 240, 240, 255}, darkBg{33, 40, 48, 255};
    CHECK(isLightBackground(lightBg));
    CHECK_FALSE(isLightBackground(darkBg));
    CHECK(adaptToBackground(white, lightBg) == black);
    CHECK(adaptToBackground(white, darkBg) == white);
    CHECK(adaptToBackground(black, darkBg) == white);
    CHECK(adaptToBackground(black, lightBg) == black);
    CHECK(adaptToBackground(red, lightBg) == red);
}

TEST_CASE("pale colours darken on white paper and deep blue lightens on a dark screen") {
    const Rgba lightBg{255, 255, 255, 255}, darkBg{33, 40, 48, 255};
    auto luma = [](Rgba c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; };
    for (Rgba pale : {Rgba{255, 255, 0, 255}, Rgba{0, 255, 255, 255}, Rgba{170, 255, 170, 255}})
        CHECK(luma(adaptToBackground(pale, lightBg)) <= 166);
    CHECK(luma(adaptToBackground(Rgba{0, 0, 255, 255}, darkBg)) >= 69);
    CHECK(adaptToBackground(Rgba{0, 0, 255, 255}, lightBg) == Rgba{0, 0, 255, 255});
}
