#include <doctest/doctest.h>

#include <cstring>

#include "tbcad/blocks/display.h"
#include "tbcad/cmd/undo.h"
#include "tbcad/text/text.h"
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

TEST_CASE(
    "an edit of a very large number of objects is built in slices, what the camera sees first") {
    constexpr int kN = 30000;
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    cmd::UndoStack undo;
    {
        auto tx = doc.begin("gen");
        for (int i = 0; i < kN; ++i) {
            const double x = (i % 200) * 100.0, y = (i / 200) * 100.0;
            (void)tx.add(Entity{{}, LineData{{x, y}, {x + 50, y + 10}}});
        }
        undo.push("gen", {tx.commit()});
    }
    REQUIRE(scene.entityCount() == kN);  // new objects are built at once
    const auto before = scene.bounds();

    // Move everything far away: the objects leave the scene and wait to be built.
    ChangeSet moved;
    {
        auto tx = doc.begin("move");
        for (EntityId id : doc.modelSpace())
            (void)tx.modify(id, [](Entity& e) {
                auto& l = std::get<LineData>(e.data);
                l.a.x += 1e6;
                l.b.x += 1e6;
            });
        moved = tx.commit();
    }
    undo.push("move", {moved});
    CHECK(sync.pendingCount() == kN);
    CHECK(scene.entityCount() == 0);

    // A camera over a corner of the moved drawing gets its objects first.
    render::Camera2D cam;
    cam.center = {1e6 + 500, 500};
    cam.pixelsPerMm = 1;
    cam.viewportW = 1000;
    cam.viewportH = 1000;
    const std::size_t built = sync.pumpPending(cam, 100000);
    CHECK(built > 0);
    CHECK(built < static_cast<std::size_t>(kN));
    CHECK(sync.pendingCount() == kN - built);
    CHECK(scene.entityCount() == built);

    // An edit of one object that is still waiting builds it right away, in its place.
    EntityId waiting;
    for (EntityId id : doc.modelSpace())
        if (!scene.contains(id)) {
            waiting = id;
            break;
        }
    {
        auto tx = doc.begin("one");
        (void)tx.modify(waiting, [](Entity& e) { std::get<LineData>(e.data).b.y += 1; });
        undo.push("one", {tx.commit()});
    }
    CHECK(scene.contains(waiting));
    CHECK(sync.pendingCount() == kN - built - 1);

    // Undoing the move while objects wait for the first one: all of them are rebuilt where they
    // were, and none is left waiting for a position that no longer exists.
    CHECK(undo.undo(doc));  // the single edit
    CHECK(undo.undo(doc));  // the move
    sync.flushPending();
    CHECK(sync.pendingCount() == 0);
    CHECK(scene.entityCount() == kN);
    const auto after = scene.bounds();
    CHECK(after.min.x == doctest::Approx(before.min.x));
    CHECK(after.max.x == doctest::Approx(before.max.x));
    CHECK(after.max.y == doctest::Approx(before.max.y));
}

TEST_CASE("FILL and QTEXT changes in the header rebuild the scene") {
    Document doc;
    render::Scene scene;
    SceneSync sync(doc, scene);
    {
        PolylineData wide;
        wide.v = {{{0, 0}, 0, 4, 4}, {{20, 0}, 0, 4, 4}};
        auto tx = doc.begin("wide");
        (void)tx.add(Entity{{}, std::move(wide)});
        tx.commit();
    }
    render::Camera2D cam;
    cam.center = {10, 0};
    cam.pixelsPerMm = 10;
    cam.viewportW = 400;
    cam.viewportH = 400;
    auto countKind = [&](render::BatchKind k) {
        int n = 0;
        for (const auto& b : scene.visibleBatches(cam))
            n += b.batch->key.kind == k ? 1 : 0;
        return n;
    };
    CHECK(countKind(render::BatchKind::Fill) > 0);
    {
        auto tx = doc.begin("fill off");
        (void)tx.modifyHeader([](Header& h) { h.custom[kFillModeKey] = "0"; });
        tx.commit();
    }
    CHECK(countKind(render::BatchKind::Fill) == 0);
    CHECK(countKind(render::BatchKind::Stroke) > 0);  // the outline
    {
        auto tx = doc.begin("fill on");
        (void)tx.modifyHeader([](Header& h) { h.custom[kFillModeKey] = "1"; });
        tx.commit();
    }
    CHECK(countKind(render::BatchKind::Fill) > 0);
}

// ---- building on several threads ---------------------------------------------------------------

namespace {

bool sameScene(const render::Scene& a, const render::Scene& b) {
    if (a.entityCount() != b.entityCount() || a.tiles().size() != b.tiles().size())
        return false;
    for (const auto& [key, ta] : a.tiles()) {
        const auto tb = b.tiles().find(key);
        if (tb == b.tiles().end() || ta.batches.size() != tb->second.batches.size())
            return false;
        for (const auto& [bk, ba] : ta.batches) {
            const auto bb = tb->second.batches.find(bk);
            if (bb == tb->second.batches.end() || ba.verts.size() != bb->second.verts.size() ||
                ba.ranges.size() != bb->second.ranges.size())
                return false;
            if (!ba.verts.empty() && std::memcmp(ba.verts.data(), bb->second.verts.data(),
                                                 ba.verts.size() * sizeof(ba.verts[0])) != 0)
                return false;
            for (std::size_t i = 0; i < ba.ranges.size(); ++i) {
                const auto &ra = ba.ranges[i], &rb = bb->second.ranges[i];
                if (ra.entity != rb.entity || ra.begin != rb.begin || ra.count != rb.count ||
                    ra.drawOrder != rb.drawOrder || ra.builtPixelsPerMm != rb.builtPixelsPerMm)
                    return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("a scene built on several threads equals the one built on one") {
    text::registerDisplay();
    blocks::registerInsertDisplay();
    Document doc;
    LayerId plain = addLayer(doc, "Plain");
    LayerId locked = addLayer(doc, "Locked");
    modifyLayer(doc, locked, [](Layer& l) { l.locked = true; });
    BlockId block;
    {
        auto tx = doc.begin("fill");
        BlockDef def;
        def.name = "SYM";
        block = tx.addBlock(def).value();
        (void)tx.add(Entity{{}, CircleData{{0, 0}, 2}}, block);
        (void)tx.add(Entity{{}, LineData{{-3, 0}, {3, 0}}}, block);
        for (int i = 0; i < 60000; ++i) {
            const double x = (i % 300) * 25.0, y = (i / 300) * 25.0;
            Entity e;
            switch (i % 8) {
                case 0:
                    e.data = LineData{{x, y}, {x + 10, y + 7}};
                    break;
                case 1: {
                    PolylineData pl;
                    pl.v = {{{x, y}, 0}, {{x + 8, y}, 0.5}, {{x + 12, y + 9}, 0}};
                    e.data = std::move(pl);
                    break;
                }
                case 2:
                    e.data = CircleData{{x, y}, 4};
                    break;
                case 3: {
                    TextData t;
                    t.position = {x, y};
                    t.height = 2.5;
                    t.utf8 = "T" + std::to_string(i);
                    e.data = std::move(t);
                    break;
                }
                case 4:
                    e.data = ArcData{{x, y}, 5, 0.2, 2.5};
                    break;
                case 5: {
                    InsertData ins;
                    ins.block = block;
                    ins.position = {x, y};
                    e.data = std::move(ins);
                    break;
                }
                default:
                    e.data = LineData{{x, y}, {x + 3, y + 3}};
                    break;
            }
            e.common.layer = (i % 5 == 0) ? locked : plain;
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    render::Scene one, many;
    SceneSync a(doc, one), b(doc, many);
    a.setMaxThreads(1);
    b.setMaxThreads(8);
    a.setPixelSize(0.5);
    b.setPixelSize(0.5);
    a.rebuildAll(500);
    b.rebuildAll(500);
    a.flushPending();  // (a big drawing is queued and built in the frames after)
    b.flushPending();
    CHECK(one.entityCount() == 60000);
    CHECK(sameScene(one, many));
}
