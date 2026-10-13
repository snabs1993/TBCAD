#include <doctest/doctest.h>

#include "tbcad/render/camera.h"

using namespace tbcad;
using namespace tbcad::render;

namespace {
Camera2D makeCamera() {
    Camera2D c;
    c.center = {100, 50};
    c.pixelsPerMm = 4;
    c.viewportW = 800;
    c.viewportH = 600;
    return c;
}
}  // namespace

TEST_CASE("screen and world coordinates round-trip") {
    Camera2D c = makeCamera();
    // Viewport centre is the camera centre; y flips.
    Vec2 mid = c.worldToScreen(c.center);
    CHECK(mid.x == doctest::Approx(400));
    CHECK(mid.y == doctest::Approx(300));
    Vec2 up = c.worldToScreen({100, 51});
    CHECK(up.y == doctest::Approx(296));
    Vec2 w = c.screenToWorld({123.5, 456.25});
    Vec2 s = c.worldToScreen(w);
    CHECK(s.x == doctest::Approx(123.5));
    CHECK(s.y == doctest::Approx(456.25));
}

TEST_CASE("zoomAt keeps the point under the cursor fixed") {
    Camera2D c = makeCamera();
    const Vec2 cursor{650, 120};
    const Vec2 before = c.screenToWorld(cursor);
    for (double f : {2.0, 0.5, 1.37, 8.0}) {
        c.zoomAt(cursor, f);
        Vec2 after = c.screenToWorld(cursor);
        CHECK(after.x == doctest::Approx(before.x).epsilon(1e-12));
        CHECK(after.y == doctest::Approx(before.y).epsilon(1e-12));
    }
}

TEST_CASE("zoomAt is precise at far coordinates") {
    Camera2D c;
    c.center = {1e8 + 0.123, -1e8};
    c.pixelsPerMm = 1000;
    c.viewportW = 1920;
    c.viewportH = 1080;
    const Vec2 cursor{1000, 400};
    const Vec2 before = c.screenToWorld(cursor);
    c.zoomAt(cursor, 1.1);
    const Vec2 after = c.screenToWorld(cursor);
    // Doubles carry ~1.5e-8 mm at 1e8; that is 1.5e-5 px at 1000 px/mm.
    CHECK(std::abs(after.x - before.x) * c.pixelsPerMm < 1e-3);
    CHECK(std::abs(after.y - before.y) * c.pixelsPerMm < 1e-3);
}

TEST_CASE("zoom is clamped and bad factors are ignored") {
    Camera2D c = makeCamera();
    c.zoomAt({0, 0}, 1e30);
    CHECK(c.pixelsPerMm == Camera2D::kMaxPixelsPerMm);
    c.zoomAt({0, 0}, 1e-60);
    CHECK(c.pixelsPerMm == Camera2D::kMinPixelsPerMm);
    Camera2D d = makeCamera();
    d.zoomAt({0, 0}, 0);
    d.zoomAt({0, 0}, -2);
    d.zoomAt({0, 0}, std::nan(""));
    CHECK(d == makeCamera());
}

TEST_CASE("panPixels moves content with the drag") {
    Camera2D c = makeCamera();
    const Vec2 world{120, 70};
    const Vec2 s0 = c.worldToScreen(world);
    c.panPixels(30, -20);
    const Vec2 s1 = c.worldToScreen(world);
    CHECK(s1.x == doctest::Approx(s0.x + 30));
    CHECK(s1.y == doctest::Approx(s0.y - 20));
}

TEST_CASE("fit centres a box and leaves a margin") {
    Camera2D c = makeCamera();
    BBox2 box;
    box.expand({0, 0});
    box.expand({200, 100});
    c.fit(box, 0.1);
    CHECK(c.center.x == doctest::Approx(100));
    CHECK(c.center.y == doctest::Approx(50));
    // Width-limited: 800 px * 0.8 / 200 mm.
    CHECK(c.pixelsPerMm == doctest::Approx(3.2));
    Vec2 corner = c.worldToScreen({0, 100});
    CHECK(corner.x == doctest::Approx(80));
}

TEST_CASE("fit ignores empty boxes and handles a single point") {
    Camera2D c = makeCamera();
    c.fit(BBox2{});
    CHECK(c == makeCamera());
    BBox2 pt;
    pt.expand({5, 5});
    c.fit(pt);
    CHECK(c.center.x == 5);
    CHECK(c.pixelsPerMm == 4);
}

TEST_CASE("visibleWorld matches the viewport corners") {
    Camera2D c = makeCamera();
    BBox2 v = c.visibleWorld();
    CHECK(v.min.x == doctest::Approx(0));
    CHECK(v.max.x == doctest::Approx(200));
    CHECK(v.min.y == doctest::Approx(50 - 75));
    CHECK(v.max.y == doctest::Approx(50 + 75));
}
