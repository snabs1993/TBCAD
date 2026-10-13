#include "support.h"

using namespace tbcad;
using namespace tbcad::blocks;
using namespace tbcad::blocks::test;

TEST_CASE("rectangular array: copies on a grid, original excluded") {
    std::vector<Entity> src{circle(0, 0, 1)};
    auto r = arrayRectangular(
        src, RectArray{.rows = 2, .columns = 3, .rowSpacing = 10, .columnSpacing = 5});
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 5);
    std::vector<Vec2> centers;
    for (const Entity& e : r.value())
        centers.push_back(entityAs<CircleData>(e)->center);
    CHECK(near(centers[0], {5, 0}));
    CHECK(near(centers[1], {10, 0}));
    CHECK(near(centers[2], {0, 10}));
    CHECK(near(centers[3], {5, 10}));
    CHECK(near(centers[4], {10, 10}));
}

TEST_CASE("rectangular array honours the grid angle and negative spacing") {
    std::vector<Entity> src{circle(0, 0, 1)};
    auto r = arrayRectangular(
        src, RectArray{.rows = 1, .columns = 3, .columnSpacing = -4, .angle = kPi / 2});
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 2);
    CHECK(near(entityAs<CircleData>(r.value()[0])->center, {0, -4}));
    CHECK(near(entityAs<CircleData>(r.value()[1])->center, {0, -8}));
}

TEST_CASE("polar array of 6 around a centre") {
    std::vector<Entity> src{circle(10, 0, 1)};
    auto r = arrayPolar(src, PolarArray{.center = {0, 0}, .count = 6});
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 5);
    for (int k = 1; k < 6; ++k) {
        double a = kTwoPi * k / 6;
        CHECK(near(entityAs<CircleData>(r.value()[static_cast<std::size_t>(k - 1)])->center,
                   {10 * std::cos(a), 10 * std::sin(a)}, 1e-9));
    }
}

TEST_CASE("polar array with a partial angle puts the last item at the end of the sweep") {
    std::vector<Entity> src{circle(10, 0, 1)};
    auto r = arrayPolar(src, PolarArray{.center = {0, 0}, .count = 4, .fillAngle = kPi});
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 3);
    CHECK(near(entityAs<CircleData>(r.value()[2])->center, {-10, 0}, 1e-9));
    CHECK(near(entityAs<CircleData>(r.value()[0])->center, {5, 5 * std::sqrt(3.0)}, 1e-9));
}

TEST_CASE("polar array without rotating items keeps their orientation") {
    std::vector<Entity> src{line(10, 0, 12, 0)};
    auto r = arrayPolar(
        src, PolarArray{.center = {0, 0}, .count = 2, .fillAngle = kPi, .rotateItems = false});
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 1);
    const auto* ln = entityAs<LineData>(r.value()[0]);
    // The line's box centre (11, 0) moves to (-11, 0); the line stays horizontal.
    CHECK(near(ln->a, {-12, 0}));
    CHECK(near(ln->b, {-10, 0}));
}

TEST_CASE("array refuses silly sizes") {
    std::vector<Entity> src{circle(0, 0, 1)};
    CHECK_FALSE(arrayRectangular(src, RectArray{.rows = 0}).ok());
    CHECK_FALSE(arrayRectangular(src, RectArray{.rows = 1000, .columns = 1000}).ok());
    CHECK_FALSE(arrayPolar(src, PolarArray{.count = 0}).ok());
    CHECK_FALSE(arrayPolar(src, PolarArray{.count = 5, .fillAngle = 0}).ok());
}

TEST_CASE("array item transforms: rectangular and polar include the original") {
    ArrayInfo rect;
    rect.rows = 2;
    rect.columns = 2;
    rect.rowSpacing = 10;
    rect.columnSpacing = 5;
    auto ts = arrayItemTransforms(rect);
    REQUIRE(ts.size() == 4);
    CHECK(near(ts[0].apply({0, 0}), {0, 0}));
    CHECK(near(ts[3].apply({0, 0}), {5, 10}));

    ArrayInfo polar;
    polar.kind = ArrayInfo::Kind::Polar;
    polar.count = 4;
    polar.center = {0, 0};
    polar.rotateItems = false;
    polar.reference = {1, 0};
    ts = arrayItemTransforms(polar);
    REQUIRE(ts.size() == 4);
    CHECK(near(ts[1].apply({1, 0}), {0, 1}));
    CHECK(near(ts[1].apply({2, 0}), {1, 1}));  // not rotated: the item only moves
}

TEST_CASE("array item transforms: path items follow the curve and align to it") {
    ArrayInfo path;
    path.kind = ArrayInfo::Kind::Path;
    path.count = 3;
    path.path = {PolyVertex{{0, 0}}, PolyVertex{{10, 0}}, PolyVertex{{10, 10}}};
    path.rotateItems = true;
    path.reference = {0, 0};
    auto ts = arrayItemTransforms(path);
    REQUIRE(ts.size() == 3);
    CHECK(near(ts[0].apply({0, 0}), {0, 0}));
    CHECK(near(ts[1].apply({0, 0}), {10, 0}));
    CHECK(near(ts[2].apply({0, 0}), {10, 10}));
    // the last item has turned a quarter circle: its x axis points along the second leg
    CHECK(near(ts[2].applyVector({1, 0}), {0, 1}, 1e-9));
    CHECK(near(ts[0].applyVector({1, 0}), {1, 0}, 1e-9));

    path.rotateItems = false;
    ts = arrayItemTransforms(path);
    CHECK(near(ts[2].applyVector({1, 0}), {1, 0}));

    path.pathClosed = true;
    path.count = 4;
    ts = arrayItemTransforms(path);
    REQUIRE(ts.size() == 4);
}
