#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {

// Hover with only `modes` on; the aperture is 10 px at pixel size 1 = 10 mm.
Vec2 snapped(World& w, Vec2 cursor, const char* modes, std::optional<Vec2> base = std::nullopt,
             cmd::FilterFeedback* fb = nullptr) {
    w.set(keys::osnap, "1");
    w.set(keys::osnapModes, modes);
    return w.hover(cursor, base, 1.0, fb);
}

}  // namespace

TEST_CASE("endpoint, midpoint and center pick the expected points") {
    World w;
    w.line({0, 0}, {100, 0});
    w.circle({50, 60}, 20);
    cmd::FilterFeedback fb;
    CHECK(near(snapped(w, {96, 3}, "endpoint", {}, &fb), {100, 0}));
    CHECK(fb.marker == "endpoint");
    CHECK(fb.active);
    CHECK(near(snapped(w, {53, 4}, "midpoint", {}, &fb), {50, 0}));
    CHECK(fb.marker == "midpoint");
    CHECK(near(snapped(w, {47, 63}, "center", {}, &fb), {50, 60}));
    CHECK(fb.marker == "center");
    // Outside the aperture nothing snaps.
    CHECK(near(snapped(w, {70, 30}, "endpoint,midpoint,center"), {70, 30}));
    // A disabled mode does not snap.
    CHECK(near(snapped(w, {96, 3}, "midpoint"), {96, 3}));
}

TEST_CASE("quadrant points of a circle and an arc") {
    World w;
    w.circle({0, 0}, 50);
    CHECK(near(snapped(w, {47, 4}, "quadrant"), {50, 0}));
    CHECK(near(snapped(w, {-3, 46}, "quadrant"), {0, 50}));
}

TEST_CASE("intersection of two lines, and of a line and an arc") {
    World w;
    w.line({0, 0}, {100, 100});
    w.line({0, 100}, {100, 0});
    cmd::FilterFeedback fb;
    CHECK(near(snapped(w, {53, 46}, "intersection", {}, &fb), {50, 50}));
    CHECK(fb.marker == "intersection");

    World v;
    v.line({-100, 0}, {100, 0});
    v.circle({0, 0}, 50);
    CHECK(near(snapped(v, {48, 4}, "intersection"), {50, 0}));
    CHECK(near(snapped(v, {-52, -3}, "intersection"), {-50, 0}));
}

TEST_CASE("intersections need both curves to really cross, not just their extensions") {
    World w;
    w.line({0, 0}, {40, 0});
    w.line({50, -20}, {50, 20});
    CHECK(near(snapped(w, {47, 3}, "intersection"), {47, 3}));
}

TEST_CASE("perpendicular from the base point to a line and to a circle") {
    World w;
    w.line({0, 0}, {100, 0});
    // From (30, 40) the foot on the x axis is (30, 0).
    CHECK(near(snapped(w, {33, 4}, "perpendicular", Vec2{30, 40}), {30, 0}));
    // Without a base point perpendicular does nothing.
    CHECK(near(snapped(w, {33, 4}, "perpendicular"), {33, 4}));

    World c;
    c.circle({0, 0}, 50);
    // From (100, 0) the feet are (50, 0) and (-50, 0).
    CHECK(near(snapped(c, {52, 3}, "perpendicular", Vec2{100, 0}), {50, 0}));
    CHECK(near(snapped(c, {-48, 3}, "perpendicular", Vec2{100, 0}), {-50, 0}));
}

TEST_CASE("perpendicular onto an arc only where the arc is") {
    World w;
    // Quarter arc in the first quadrant.
    w.add(Entity{{}, ArcData{{0, 0}, 50, 0, kPi / 2}});
    CHECK(near(snapped(w, {37, 37}, "perpendicular", Vec2{100, 100}),
               {50 * std::cos(kPi / 4), 50 * std::sin(kPi / 4)}));
    // The opposite foot (225 deg) is not on the arc.
    CHECK(near(snapped(w, {-35, -35}, "perpendicular", Vec2{100, 100}), {-35, -35}));
}

TEST_CASE("tangent from an external point to a circle") {
    World w;
    w.circle({0, 0}, 30);
    // From (50, 0): tangent points at x = 18, y = +-24.
    const Vec2 up{18, 24}, down{18, -24};
    CHECK(near(snapped(w, {20, 22}, "tangent", Vec2{50, 0}), up));
    CHECK(near(snapped(w, {20, -22}, "tangent", Vec2{50, 0}), down));
    // From inside the circle there is no tangent.
    CHECK(near(snapped(w, {20, 22}, "tangent", Vec2{5, 0}), {20, 22}));
}

TEST_CASE("nearest snaps to the curve") {
    World w;
    w.line({0, 0}, {100, 0});
    cmd::FilterFeedback fb;
    CHECK(near(snapped(w, {37, 6}, "nearest", {}, &fb), {37, 0}));
    CHECK(fb.marker == "nearest");
}

TEST_CASE("priority: nearer wins, ties go to endpoint > intersection > midpoint > ... > nearest") {
    World w;
    w.line({0, 0}, {100, 0});
    w.line({100, 0}, {100, 100});
    // Cursor right at the shared corner: endpoint beats intersection and nearest.
    cmd::FilterFeedback fb;
    snapped(w, {100, 0}, "endpoint,intersection,nearest", {}, &fb);
    CHECK(fb.marker == "endpoint");
    // Slightly off the corner the nearer point on the line wins over the farther endpoint.
    snapped(w, {99, 1}, "endpoint,intersection,nearest", {}, &fb);
    CHECK(fb.marker == "nearest");

    World v;
    v.line({0, 0}, {100, 100});
    v.line({0, 100}, {100, 0});
    // Intersection point (50,50) is also the nearest-point candidate; intersection must win.
    snapped(v, {50, 50}, "nearest,intersection", {}, &fb);
    CHECK(fb.marker == "intersection");
    // A closer midpoint beats a farther endpoint.
    World m;
    m.line({0, 0}, {16, 0});
    snapped(m, {7, 0}, "endpoint,midpoint", {}, &fb);
    CHECK(fb.marker == "midpoint");
}

TEST_CASE("Tab cycles through the candidates at the cursor and wraps around") {
    World w;
    w.line({0, 0}, {100, 0});
    w.line({100, 0}, {100, 100});
    w.set(keys::osnap, "1");
    w.set(keys::osnapModes, "endpoint,intersection,nearest");
    cmd::FilterFeedback fb;
    w.hover({100, 0}, std::nullopt, 1.0, &fb);
    const auto n = w.osnap.candidates().size();
    REQUIRE(n >= 3);
    std::vector<std::string> seen{fb.marker};
    for (std::size_t i = 1; i < n; ++i) {
        w.osnap.cycle();
        w.hover({100, 0}, std::nullopt, 1.0, &fb);
        seen.push_back(fb.marker);
    }
    CHECK(seen.front() == "endpoint");
    CHECK(seen.size() == n);
    w.osnap.cycle();
    w.hover({100, 0}, std::nullopt, 1.0, &fb);
    CHECK(fb.marker == "endpoint");  // wrapped around
    // Moving to a place with different candidates starts over.
    w.osnap.cycle();
    w.hover({50, 1}, std::nullopt, 1.0, &fb);
    CHECK(fb.marker == "nearest");
}

TEST_CASE("OSNAP off, layers off and invisible entities do not snap") {
    World w;
    w.line({0, 0}, {100, 0});
    w.set(keys::osnapModes, "endpoint");
    w.set(keys::osnap, "0");
    CHECK(near(w.hover({98, 2}), {98, 2}));
    CHECK(near(snapped(w, {98, 2}, "endpoint"), {100, 0}));

    auto tx = w.doc.begin("hide");
    REQUIRE(tx.modify(w.idx.queryIds(BBox2{{-1, -1}, {101, 1}})[0],
                      [](Entity& e) { e.common.visible = false; }));
    (void)tx.commit();
    CHECK(near(snapped(w, {98, 2}, "endpoint"), {98, 2}));
}

TEST_CASE("the aperture scales with the pixel size") {
    World w;
    w.line({0, 0}, {100, 0});
    w.set(keys::osnap, "1");
    w.set(keys::osnapModes, "endpoint");
    CHECK(near(w.hover({105, 5}, std::nullopt, 1.0), {100, 0}));  // 7 mm away, aperture 10
    CHECK(near(w.hover({105, 5}, std::nullopt, 0.1), {105, 5}));  // aperture 1 mm
}

TEST_CASE("at most 200 entities are examined, nearest first") {
    World w;
    for (int i = 0; i < 300; ++i)
        w.line({0.0, 5.0 + i * 0.01}, {100, 5.0 + i * 0.01});  // all within the aperture
    // The marker line is the nearest of all and must be among the examined ones.
    w.line({0, 0.1}, {100, 0.1});
    auto res = findSnapCandidates(
        w.doc, w.idx, SnapRequest{{0, 0.2}, std::nullopt, 10, maskOf(SnapMode::Endpoint), 200});
    REQUIRE_FALSE(res.empty());
    CHECK(near(res.front().point, {0, 0.1}));
}

TEST_CASE("a geometry provider registered for another kind is used (blocks, later)") {
    SnapGeometryRegistry reg;
    reg.set(EntityKind::Insert, [](const Document&, const Entity&, SnapGeometry& out) {
        out.endpoints.push_back({7, 7});
    });
    Document doc;
    index::SpatialIndex idx;
    index::DocumentIndexer indexer(doc, idx, [&](const Entity& e) { return geom::bounds(e, doc); });
    // The geom bounds of an insert need the document's block; index the box by hand.
    EntityId id;
    {
        auto tx = doc.begin("x");
        auto r = tx.add(Entity{{}, PointData{{7, 7}}});
        id = r.value();
        (void)tx.commit();
    }
    (void)idx;
    // Replace the point provider with the insert one for the same entity.
    reg.set(EntityKind::Point, [](const Document&, const Entity&, SnapGeometry& out) {
        out.endpoints.push_back({8, 8});
    });
    auto res = findSnapCandidates(
        doc, idx, SnapRequest{{7, 7}, std::nullopt, 5, maskOf(SnapMode::Endpoint)}, reg);
    REQUIRE(res.size() == 1);
    CHECK(near(res[0].point, {8, 8}));
    CHECK(res[0].entity == id);
}

TEST_CASE("point entities snap as nodes") {
    World w;
    w.add(Entity{{}, PointData{{25, 25}}});
    CHECK(near(snapped(w, {27, 24}, "node"), {25, 25}));
}
