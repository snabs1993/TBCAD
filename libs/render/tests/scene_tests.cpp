#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "tbcad/render/scene.h"

using namespace tbcad;
using namespace tbcad::render;

namespace {

constexpr Rgba kRed{255, 0, 0, 255};
constexpr Rgba kGreen{0, 255, 0, 255};

DisplayStyle style(Rgba color = kRed, double lw = 0, LayerId layer = {1}) {
    DisplayStyle s;
    s.color = color;
    s.lineweight = lw;
    s.layer = layer;
    return s;
}

DisplayList lineList(Vec2 a, Vec2 b, DisplayStyle st = style()) {
    DisplayList l;
    l.items.push_back(DisplayPolyline{{a, b}, false, st});
    return l;
}

Camera2D cameraAt(Vec2 center, double ppmm = 1, int w = 200, int h = 100) {
    Camera2D c;
    c.center = center;
    c.pixelsPerMm = ppmm;
    c.viewportW = w;
    c.viewportH = h;
    return c;
}

std::size_t totalVertices(const Scene& s) {
    std::size_t n = 0;
    for (const auto& [tk, tile] : s.tiles())
        for (const auto& [bk, b] : tile.batches)
            n += b.verts.size();
    return n;
}

Vec2 worldOf(const Tile& t, const GpuVertex& v) {
    return {t.origin.x + static_cast<double>(v.hx) + static_cast<double>(v.lx),
            t.origin.y + static_cast<double>(v.hy) + static_cast<double>(v.ly)};
}

}  // namespace

TEST_CASE("tile assignment follows the segment start and negative coordinates floor") {
    Scene s({.tileSizeMm = 100});
    CHECK(s.tileOf({0, 0}) == TileKey{0, 0});
    CHECK(s.tileOf({99.9, 100}) == TileKey{0, 1});
    CHECK(s.tileOf({-0.1, -100.1}) == TileKey{-1, -2});

    s.upsert({1}, lineList({150, 50}, {250, 60}), 0);  // starts in tile (1,0), ends in (2,0)
    REQUIRE(s.tiles().size() == 1);
    const Tile& t = s.tiles().begin()->second;
    CHECK(t.key == TileKey{1, 0});
    CHECK(t.origin.x == 100);
    // Bounds cover the whole segment, not just the cell.
    CHECK(t.geometryBounds().max.x == doctest::Approx(250));
}

TEST_CASE("vertices reproduce world positions at far coordinates") {
    Scene s({.tileSizeMm = 1024});
    const Vec2 a{1e8 + 0.123456789, -1e8 - 5.5};
    const Vec2 b{1e8 + 900.987654321, -1e8 + 30.25};
    s.upsert({1}, lineList(a, b), 0);
    const Tile& t = s.tiles().begin()->second;
    const Batch& batch = t.batches.begin()->second;
    REQUIRE(batch.verts.size() == 2);
    Vec2 ra = worldOf(t, batch.verts[0]);
    Vec2 rb = worldOf(t, batch.verts[1]);
    CHECK(std::abs(ra.x - a.x) < 1e-6);
    CHECK(std::abs(ra.y - a.y) < 1e-6);
    CHECK(std::abs(rb.x - b.x) < 1e-6);
    CHECK(std::abs(rb.y - b.y) < 1e-6);
}

TEST_CASE("entities with the same style share a batch - different styles do not") {
    Scene s;
    s.upsert({1}, lineList({0, 0}, {1, 0}), 0);
    s.upsert({2}, lineList({0, 1}, {1, 1}), 1);
    s.upsert({3}, lineList({0, 2}, {1, 2}, style(kGreen)), 2);
    REQUIRE(s.tiles().size() == 1);
    const Tile& t = s.tiles().begin()->second;
    CHECK(t.batches.size() == 2);
    for (const auto& [k, b] : t.batches) {
        if (k.rgba == 0x000000FFu + 0xFF000000u)
            CHECK(b.verts.size() == 4);
    }
    CHECK(s.entityCount() == 3);
    CHECK(totalVertices(s) == 6);
}

TEST_CASE("remove drops exactly that entity and frees empty batches") {
    Scene s;
    s.upsert({1}, lineList({0, 0}, {1, 0}), 0);
    s.upsert({2}, lineList({0, 1}, {1, 1}), 1);
    s.upsert({3}, lineList({0, 2}, {1, 2}, style(kGreen)), 2);
    (void)s.takeDeletedBatches();

    s.remove({1});
    CHECK_FALSE(s.contains({1}));
    CHECK(totalVertices(s) == 4);
    const Tile& t = s.tiles().begin()->second;
    for (const auto& [k, b] : t.batches) {
        for (const BatchRange& r : b.ranges)
            CHECK(r.entity != EntityId{1});
        // Remaining ranges still point at the right vertices.
        for (const BatchRange& r : b.ranges)
            CHECK(r.begin + r.count <= b.verts.size());
    }
    CHECK(s.takeDeletedBatches().empty());

    s.remove({3});  // the green batch becomes empty
    CHECK(s.takeDeletedBatches().size() == 1);
    s.remove({2});
    CHECK(s.tiles().empty());
    CHECK(s.bounds().empty());
    s.remove({2});  // unknown id is fine
}

TEST_CASE("upsert replaces an entity's geometry") {
    Scene s({.tileSizeMm = 100});
    s.upsert({1}, lineList({0, 0}, {10, 0}), 0);
    s.upsert({1}, lineList({500, 500}, {510, 500}), 0);
    CHECK(s.entityCount() == 1);
    REQUIRE(s.tiles().size() == 1);
    CHECK(s.tiles().begin()->first == TileKey{5, 5});
    BBox2 b = s.bounds();
    CHECK(b.min.x == doctest::Approx(500));
    CHECK(b.max.x == doctest::Approx(510));
}

TEST_CASE("removing from the middle of a batch keeps the other ranges intact") {
    Scene s;
    for (std::uint64_t i = 1; i <= 5; ++i)
        s.upsert({i}, lineList({double(i), 0}, {double(i), 1}), 0);
    s.remove({2});
    s.remove({4});
    const Tile& t = s.tiles().begin()->second;
    const Batch& b = t.batches.begin()->second;
    REQUIRE(b.ranges.size() == 3);
    for (const BatchRange& r : b.ranges) {
        double x = worldOf(t, b.verts[r.begin]).x;
        CHECK(x == doctest::Approx(static_cast<double>(r.entity.value)));
    }
}

TEST_CASE("dash distances accumulate along a polyline and wrap by the period") {
    const std::vector<double> pattern = {6, -2};  // period 8
    DisplayStyle st = style();
    st.pattern = &pattern;
    DisplayList l;
    l.items.push_back(DisplayPolyline{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, true, st});
    Scene s;
    s.upsert({1}, l, 0);
    const Tile& t = s.tiles().begin()->second;
    const Batch& b = t.batches.begin()->second;
    // Closed polyline: 4 segments of length 10 -> 8 vertices.
    REQUIRE(b.verts.size() == 8);
    const double expectStart[4] = {0, 2, 4, 6};  // 0, 10, 20, 30 mod 8
    for (int i = 0; i < 4; ++i) {
        CHECK(b.verts[2 * i].d == doctest::Approx(expectStart[i]));
        CHECK(b.verts[2 * i + 1].d == doctest::Approx(expectStart[i] + 10));
    }
}

TEST_CASE("pattern scale multiplies the pattern") {
    const std::vector<double> pattern = {1, -1};
    DisplayStyle st = style();
    st.pattern = &pattern;
    st.patternScale = 5;
    Scene s;
    s.upsert({1}, lineList({0, 0}, {100, 0}, st), 0);
    const Batch& b = s.tiles().begin()->second.batches.begin()->second;
    const DashPattern& p = s.pattern(b.key.pattern);
    CHECK(p.period() == doctest::Approx(10));
    CHECK(b.verts[1].d == doctest::Approx(100));  // 0 + 100, not wrapped within one piece
}

TEST_CASE("very long dashed segments are split so dash distances stay small") {
    const std::vector<double> pattern = {1, -1};
    DisplayStyle st = style();
    st.pattern = &pattern;
    Scene s({.tileSizeMm = 1e7});
    s.upsert({1}, lineList({0, 0}, {5e6, 0}, st), 0);
    const Batch& b = s.tiles().begin()->second.batches.begin()->second;
    CHECK(b.verts.size() > 2);
    for (const GpuVertex& v : b.verts)
        CHECK(v.d < 2100);  // at most 1000 periods per piece
    const Tile& t = s.tiles().begin()->second;
    CHECK(worldOf(t, b.verts.back()).x == doctest::Approx(5e6));
}

TEST_CASE("continuous styles store no dash distance") {
    Scene s;
    s.upsert({1}, lineList({0, 0}, {10, 0}), 0);
    const Batch& b = s.tiles().begin()->second.batches.begin()->second;
    CHECK(b.verts[0].d == 0);
    CHECK(b.verts[1].d == 0);
}

TEST_CASE("fills are triangulated into the fill kind") {
    DisplayFill f;
    f.rings = {{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{2, 2}, {4, 2}, {4, 4}, {2, 4}}};
    f.style = style(kGreen);
    DisplayList l;
    l.items.push_back(f);
    Scene s;
    s.upsert({1}, l, 0);
    const Tile& t = s.tiles().begin()->second;
    REQUIRE(t.batches.size() == 1);
    const Batch& b = t.batches.begin()->second;
    CHECK(b.key.kind == BatchKind::Fill);
    CHECK(b.verts.size() % 3 == 0);
    double area = 0;
    for (std::size_t i = 0; i + 2 < b.verts.size(); i += 3) {
        Vec2 p0 = worldOf(t, b.verts[i]), p1 = worldOf(t, b.verts[i + 1]),
             p2 = worldOf(t, b.verts[i + 2]);
        area += std::abs(cross(p1 - p0, p2 - p0)) * 0.5;
    }
    CHECK(area == doctest::Approx(100 - 4));
}

TEST_CASE("points become one vertex each") {
    DisplayPoints pts;
    pts.pts = {{1, 1}, {2, 2}, {3, 3}};
    pts.style = style();
    DisplayList l;
    l.items.push_back(pts);
    Scene s;
    s.upsert({1}, l, 0);
    CHECK(totalVertices(s) == 3);
    CHECK(s.tiles().begin()->second.batches.begin()->first.kind == BatchKind::Point);
}

TEST_CASE("non-finite and degenerate geometry is skipped") {
    DisplayList l;
    l.items.push_back(DisplayPolyline{{{0, 0}, {0, 0}}, false, style()});             // zero length
    l.items.push_back(DisplayPolyline{{{0, 0}, {std::nan(""), 1}}, false, style()});  // NaN
    l.items.push_back(DisplayPolyline{{{0, 0}, {INFINITY, 1}}, false, style()});      // inf
    l.items.push_back(DisplayPolyline{{{0, 0}}, false, style()});                     // one point
    l.items.push_back(DisplayPolyline{{{0, 0}, {1e30, 0}}, false, style()});          // absurd
    Scene s;
    s.upsert({1}, l, 0);
    CHECK(totalVertices(s) == 0);
    CHECK(s.contains({1}));
    s.remove({1});
    CHECK(s.entityCount() == 0);
}

TEST_CASE("visibleBatches culls by geometry bounds and honours layer visibility") {
    Scene s({.tileSizeMm = 100});
    s.upsert({1}, lineList({0, 0}, {10, 0}), 0);                        // tile (0,0)
    s.upsert({2}, lineList({1000, 1000}, {1010, 1000}), 0);             // tile (10,10)
    s.upsert({3}, lineList({5, 5}, {6, 5}, style(kGreen, 0, {2})), 0);  // layer 2

    auto near = s.visibleBatches(cameraAt({5, 0}));
    CHECK(near.size() == 2);
    auto far = s.visibleBatches(cameraAt({1005, 1000}));
    CHECK(far.size() == 1);
    CHECK(s.visibleBatches(cameraAt({50000, 50000})).empty());

    s.setLayerVisible({2}, false);
    CHECK_FALSE(s.layerVisible({2}));
    CHECK(s.visibleBatches(cameraAt({5, 0})).size() == 1);
    s.setLayerVisible({2}, true);
    CHECK(s.visibleBatches(cameraAt({5, 0})).size() == 2);
}

TEST_CASE("a long segment stays visible while its start tile is off screen") {
    Scene s({.tileSizeMm = 100});
    s.upsert({1}, lineList({0, 0}, {10000, 0}), 0);
    // The camera looks at x = 5000, far from the start tile.
    CHECK(s.visibleBatches(cameraAt({5000, 0})).size() == 1);
}

TEST_CASE("thick lines stay visible when only their width reaches the view") {
    Scene s({.tileSizeMm = 100});
    s.upsert({1}, lineList({0, 0}, {10, 0}, style(kRed, 4.0)), 0);  // 4 mm wide: edge at y = 2
    // 100 px at 100 px/mm = a 1 mm tall view.
    Camera2D c = cameraAt({5, 2.4}, 100, 100, 100);  // sees y in [1.9, 2.9]
    CHECK(s.visibleBatches(c).size() == 1);
    c.center.y = 3.5;  // sees [3.0, 4.0]
    CHECK(s.visibleBatches(c).empty());
}

TEST_CASE("draw order sorts fills, strokes and points, then drawOrder") {
    DisplayList fill;
    DisplayFill f;
    f.rings = {{{0, 0}, {5, 0}, {5, 5}}};
    f.style = style(kGreen);
    fill.items.push_back(f);
    DisplayPoints p;
    p.pts = {{1, 1}};
    p.style = style();
    DisplayList points;
    points.items.push_back(p);

    Scene s;
    s.upsert({1}, points, 0);
    s.upsert({2}, lineList({0, 0}, {5, 5}, style(kRed)), 9);
    s.upsert({3}, lineList({0, 1}, {5, 6}, style(kGreen)), 3);
    s.upsert({4}, fill, 7);
    auto v = s.visibleBatches(cameraAt({2, 2}));
    REQUIRE(v.size() == 4);
    CHECK(v[0].batch->key.kind == BatchKind::Fill);
    CHECK(v[1].batch->key.kind == BatchKind::Stroke);
    CHECK(v[1].batch->minDrawOrder == 3);
    CHECK(v[2].batch->minDrawOrder == 9);
    CHECK(v[3].batch->key.kind == BatchKind::Point);
}

TEST_CASE("batch versions change whenever geometry changes") {
    Scene s;
    s.upsert({1}, lineList({0, 0}, {1, 0}), 0);
    const Batch* b = &s.tiles().begin()->second.batches.begin()->second;
    auto v0 = b->version;
    s.upsert({2}, lineList({0, 1}, {1, 1}), 0);
    b = &s.tiles().begin()->second.batches.begin()->second;
    CHECK(b->version > v0);
    auto v1 = b->version;
    s.remove({2});
    b = &s.tiles().begin()->second.batches.begin()->second;
    CHECK(b->version > v1);
}

TEST_CASE("retessellation hint triggers beyond 4x zoom") {
    Camera2D a = cameraAt({0, 0}, 1);
    Camera2D b = a;
    b.pixelsPerMm = 3.9;
    CHECK_FALSE(Scene::needsRetessellation(a, b));
    b.pixelsPerMm = 4.1;
    CHECK(Scene::needsRetessellation(a, b));
    b.pixelsPerMm = 0.26;
    CHECK_FALSE(Scene::needsRetessellation(a, b));
    b.pixelsPerMm = 0.24;
    CHECK(Scene::needsRetessellation(a, b));
    b = a;
    b.center = {1e6, 1e6};  // panning alone never matters
    CHECK_FALSE(Scene::needsRetessellation(a, b));
}

TEST_CASE("staleEntities reports only visible geometry built for another zoom") {
    Scene s({.tileSizeMm = 100});
    s.upsert({1}, lineList({0, 0}, {10, 0}), 0, 1.0);  // built at 1 px/mm
    s.upsert({2}, lineList({5, 1}, {15, 1}, style(kGreen)), 0, 8.0);
    s.upsert({3}, lineList({5, 2}, {15, 2}, style(kGreen)), 0);  // unknown zoom
    s.upsert({4}, lineList({5000, 0}, {5010, 0}), 0, 1.0);       // off screen
    Camera2D cam = cameraAt({5, 0}, 8.0, 80, 80);
    auto stale = s.staleEntities(cam);
    REQUIRE(stale.size() == 1);
    CHECK(stale[0] == EntityId{1});
    s.upsert({1}, lineList({0, 0}, {10, 0}), 0, 8.0);  // rebuilt for the current zoom
    CHECK(s.staleEntities(cam).empty());
}

TEST_CASE("selection geometry copies the strokes of selected entities only") {
    Scene s({.tileSizeMm = 100});
    s.upsert({1}, lineList({0, 0}, {10, 0}), 0);
    s.upsert({2}, lineList({0, 1}, {10, 1}), 0);
    s.upsert({3}, lineList({500, 0}, {510, 0}, style(kGreen, 0.5)), 0);
    EntityId sel[] = {{1}, {3}};
    s.setSelected(sel);
    const SelectionGeometry& g = s.selectedGeometry();
    std::size_t verts = 0;
    for (const auto& t : g.tiles)
        verts += t.verts.size();
    CHECK(verts == 4);
    CHECK(g.tiles.size() == 2);  // different tiles
    CHECK(s.highlightGeometry().tiles.empty());

    auto v = g.version;
    s.remove({1});  // geometry follows scene changes
    CHECK(s.selectedGeometry().version != v);
    verts = 0;
    for (const auto& t : s.selectedGeometry().tiles)
        verts += t.verts.size();
    CHECK(verts == 2);

    s.setLayerVisible({1}, false);  // hidden layers are not highlighted
    CHECK(s.selectedGeometry().tiles.empty());
}

TEST_CASE("suggestTileSize gives a power of two for roughly 8 tiles across") {
    BBox2 b;
    b.expand({0, 0});
    b.expand({100000, 50000});
    double t = Scene::suggestTileSize(b);
    CHECK(t == 16384);  // 100000 / 8 = 12500 -> 16384
    CHECK(Scene::suggestTileSize(BBox2{}) == 1024);
    BBox2 tiny;
    tiny.expand({0, 0});
    tiny.expand({1, 1});
    CHECK(Scene::suggestTileSize(tiny) == 16);
}

TEST_CASE("clear removes everything and reports the freed batches") {
    Scene s;
    s.upsert({1}, lineList({0, 0}, {1, 1}), 0);
    s.upsert({2}, lineList({0, 0}, {1, 1}, style(kGreen)), 0);
    (void)s.takeDeletedBatches();
    s.clear();
    CHECK(s.entityCount() == 0);
    CHECK(s.tiles().empty());
    CHECK(s.takeDeletedBatches().size() == 2);
}

TEST_CASE("a scene of 100k lines builds and culls quickly enough to be useful") {
    Scene s({.tileSizeMm = 1024});
    for (std::uint64_t i = 1; i <= 100000; ++i) {
        double x = static_cast<double>((i * 7919) % 100000);
        double y = static_cast<double>((i * 104729) % 100000);
        s.upsert({i}, lineList({x, y}, {x + 20, y + 10}), static_cast<std::uint32_t>(i));
    }
    CHECK(s.entityCount() == 100000);
    CHECK(totalVertices(s) == 200000);
    Camera2D cam = cameraAt({50000, 50000}, 0.01, 1000, 1000);
    auto all = s.visibleBatches(cam);
    CHECK(all.size() > 1);
    Camera2D zoomed = cameraAt({100, 100}, 1, 100, 100);
    CHECK(s.visibleBatches(zoomed).size() < all.size());
}
