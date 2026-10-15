#include <doctest/doctest.h>

#include "tbcad/cmd/undo.h"
#include "tbcad/ui/scene_sync.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;

TEST_CASE("scene follows adds, edits and erases") {
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    EntityId a = addLine(doc, {0, 0}, {10, 0});
    EntityId b = addLine(doc, {0, 5}, {10, 5});
    CHECK(scene.contains(a));
    CHECK(scene.contains(b));
    CHECK(scene.entityCount() == 2);
    {
        auto tx = doc.begin("erase");
        (void)tx.erase(a);
        tx.commit();
    }
    CHECK_FALSE(scene.contains(a));
    CHECK(scene.entityCount() == 1);
    CHECK_FALSE(scene.bounds().empty());
}

TEST_CASE("undo and redo are mirrored into the scene") {
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    cmd::UndoStack undo;
    ChangeSet cs;
    {
        auto tx = doc.begin("line");
        (void)tx.add(Entity{{}, LineData{{0, 0}, {1, 1}}});
        cs = tx.commit();
    }
    undo.push("line", {cs});
    CHECK(scene.entityCount() == 1);
    CHECK(undo.undo(doc));
    CHECK(scene.entityCount() == 0);
    CHECK(undo.redo(doc));
    CHECK(scene.entityCount() == 1);
}

TEST_CASE("layer visibility goes through the effective state") {
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    LayerId parent = addLayer(doc, "P");
    LayerId child = addLayer(doc, "C", parent);
    EntityId e = addLine(doc, {0, 0}, {10, 0}, child);
    (void)e;
    CHECK(scene.layerVisible(child));
    modifyLayer(doc, parent, [](Layer& l) { l.on = false; });
    CHECK_FALSE(scene.layerVisible(parent));
    CHECK_FALSE(scene.layerVisible(child));
    modifyLayer(doc, parent, [](Layer& l) {
        l.on = true;
        l.frozen = true;
    });
    CHECK_FALSE(scene.layerVisible(child));
    modifyLayer(doc, parent, [](Layer& l) { l.frozen = false; });
    CHECK(scene.layerVisible(child));
}

TEST_CASE("entities on a locked layer are drawn faded and back to normal when unlocked") {
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    LayerId layer = addLayer(doc, "L");
    addLine(doc, {0, 0}, {10, 0}, layer);
    auto alphaOf = [&] {
        for (const auto& [key, tile] : scene.tiles())
            for (const auto& [bk, batch] : tile.batches)
                return static_cast<int>(bk.rgba >> 24);
        return -1;
    };
    CHECK(alphaOf() == 255);
    modifyLayer(doc, layer, [](Layer& l) { l.locked = true; });
    CHECK(alphaOf() < 255);
    CHECK(alphaOf() > 0);
    modifyLayer(doc, layer, [](Layer& l) { l.locked = false; });
    CHECK(alphaOf() == 255);
}

TEST_CASE("rebuildAll mirrors an existing document") {
    Document doc;
    addLine(doc, {0, 0}, {10, 0});
    addLine(doc, {0, 5}, {10, 5});
    render::Scene scene;
    SceneSync sync(doc, scene);
    CHECK(scene.entityCount() == 0);
    sync.rebuildAll();
    CHECK(scene.entityCount() == 2);
}

TEST_CASE("circles are retessellated when the zoom changes a lot, lines are not") {
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    sync.setPixelSize(1.0);
    {
        auto tx = doc.begin("c");
        (void)tx.add(Entity{{}, CircleData{{0, 0}, 100}});
        (void)tx.add(Entity{{}, LineData{{0, 0}, {50, 50}}});
        tx.commit();
    }
    render::Camera2D cam;
    cam.viewportW = cam.viewportH = 1000;
    cam.pixelsPerMm = 1.0;
    CHECK(sync.refreshStale(cam) == 0);
    cam.pixelsPerMm = 50.0;  // 50x closer
    CHECK(sync.refreshStale(cam) == 1);
    CHECK(sync.refreshStale(cam) == 0);
}

TEST_CASE("levels: other levels are hidden or faded, hidden levels leave the scene") {
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    EntityId a = addLine(doc, {0, 0}, {10, 0});  // first level
    LevelId upper;
    {
        auto tx = doc.begin("level");
        Level l;
        l.name = "Upper";
        upper = tx.addLevel(l).value();
        REQUIRE(tx.modifyHeader([&](Header& h) { h.currentLevel = upper; }).ok());
        tx.commit();
    }
    EntityId b = addLine(doc, {0, 0}, {10, 0});  // same place, upper level
    CHECK(scene.contains(a));                    // dimmed view: still drawn
    CHECK(scene.contains(b));
    {
        auto tx = doc.begin("hide others");
        REQUIRE(tx.modifyHeader([&](Header& h) { h.levelView = LevelView::HideOthers; }).ok());
        tx.commit();
    }
    CHECK_FALSE(scene.contains(a));
    CHECK(scene.contains(b));
    {
        auto tx = doc.begin("dim others");
        REQUIRE(tx.modifyHeader([&](Header& h) { h.levelView = LevelView::DimOthers; }).ok());
        REQUIRE(tx.modifyLevel(upper, [](Level& l) { l.locked = true; }).ok());
        tx.commit();
    }
    CHECK(scene.contains(a));
    {
        auto tx = doc.begin("hide first");
        REQUIRE(tx.modifyLevel(kLevelDefault, [](Level& l) { l.visible = false; }).ok());
        tx.commit();
    }
    CHECK_FALSE(scene.contains(a));
    CHECK(scene.contains(b));
}

TEST_CASE("a wipeout is drawn between the objects below it and the ones above it") {
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    addLine(doc, {0, 0}, {10, 10});
    {
        PolylineData box;
        for (Vec2 p : {Vec2{2, 2}, Vec2{8, 2}, Vec2{8, 8}, Vec2{2, 8}})
            box.v.push_back({p});
        box.closed = true;
        Entity e{{}, std::move(box)};
        e.common.wipeout = true;
        auto tx = doc.begin("wipeout");
        (void)tx.add(std::move(e));
        tx.commit();
    }
    addLine(doc, {0, 10}, {10, 0});  // added after the wipeout: stays on top of it
    render::Camera2D cam;
    cam.center = {5, 5};
    cam.pixelsPerMm = 10;
    cam.viewportW = 400;
    cam.viewportH = 400;
    auto v = scene.visibleBatches(cam);
    // Same layer and style: two stroke batches, one on each side of the mask.
    std::vector<render::BatchKind> kinds;
    for (const auto& b : v)
        kinds.push_back(b.batch->key.kind);
    REQUIRE(kinds.size() == 3);
    CHECK(kinds[0] == render::BatchKind::Stroke);
    CHECK(kinds[1] == render::BatchKind::Mask);
    CHECK(kinds[2] == render::BatchKind::Stroke);
    CHECK_FALSE(scene.needsRelevel());
}
