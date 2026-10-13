#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "tbcad/render/overlay.h"

using namespace tbcad;
using namespace tbcad::render;

namespace {

Camera2D camera() {
    Camera2D c;
    c.center = {0, 0};
    c.pixelsPerMm = 10;
    c.viewportW = 400;
    c.viewportH = 300;
    return c;
}

struct Bounds {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
};
Bounds boundsOf(const OverlayMesh& m) {
    Bounds b;
    for (const auto& v : m.vertices) {
        b.x0 = std::min(b.x0, v.x);
        b.y0 = std::min(b.y0, v.y);
        b.x1 = std::max(b.x1, v.x);
        b.y1 = std::max(b.y1, v.y);
    }
    return b;
}

}  // namespace

TEST_CASE("an empty overlay builds an empty mesh") {
    Overlay o;
    CHECK(o.empty());
    CHECK(o.build(camera()).vertices.empty());
}

TEST_CASE("a world-space line lands at the projected screen position") {
    Overlay o;
    o.line(Space::World, {0, 0}, {10, 0}, OverlayStyle{{255, 0, 0, 255}, 1.0, 0, 0});
    OverlayMesh m = o.build(camera());
    REQUIRE(m.vertices.size() == 6);  // one quad
    Bounds b = boundsOf(m);
    CHECK(b.x0 == doctest::Approx(200));
    CHECK(b.x1 == doctest::Approx(300));
    CHECK(b.y0 == doctest::Approx(149.5));
    CHECK(b.y1 == doctest::Approx(150.5));
    CHECK(m.vertices[0].r == 255);
    CHECK(m.vertices[0].g == 0);
}

TEST_CASE("screen-space shapes ignore the camera") {
    Overlay o;
    o.line(Space::Screen, {10, 10}, {60, 10}, OverlayStyle{});
    Camera2D c = camera();
    Bounds b1 = boundsOf(o.build(c));
    c.center = {12345, -999};
    c.pixelsPerMm = 0.01;
    Bounds b2 = boundsOf(o.build(c));
    CHECK(b1.x0 == b2.x0);
    CHECK(b1.x1 == b2.x1);
}

TEST_CASE("world-space overlays are exact at far coordinates") {
    Camera2D c = camera();
    c.center = {1e8 + 0.25, -1e8};
    c.pixelsPerMm = 1000;
    Overlay o;
    o.line(Space::World, {c.center.x - 0.1, c.center.y}, {c.center.x + 0.1, c.center.y},
           OverlayStyle{});
    Bounds b = boundsOf(o.build(c));
    CHECK(b.x0 == doctest::Approx(100).epsilon(1e-4));  // 200 - 0.1 mm * 1000
    CHECK(b.x1 == doctest::Approx(300).epsilon(1e-4));
}

TEST_CASE("dashed lines are emitted as separate dashes with the right coverage") {
    Overlay o;
    o.line(Space::Screen, {0, 50}, {100, 50}, OverlayStyle{{0, 255, 0, 255}, 1.0, 6.0, 4.0});
    OverlayMesh m = o.build(camera());
    // 100 px with a 10 px period = 10 dashes of 6 px.
    CHECK(m.vertices.size() == 10 * 6);
    double covered = 0;
    for (std::size_t i = 0; i + 5 < m.vertices.size(); i += 6) {
        float x0 = 1e30f, x1 = -1e30f;
        for (int k = 0; k < 6; ++k) {
            x0 = std::min(x0, m.vertices[i + k].x);
            x1 = std::max(x1, m.vertices[i + k].x);
        }
        covered += x1 - x0;
    }
    CHECK(covered == doctest::Approx(60));
}

TEST_CASE("dash phase continues across polyline vertices") {
    Overlay o;
    const Vec2 pts[] = {{0, 0}, {25, 0}, {25, 25}};
    o.polyline(Space::Screen, pts, false, OverlayStyle{{255, 255, 255, 255}, 1.0, 5.0, 5.0});
    OverlayMesh m = o.build(camera());
    // Total length 50 px at a 10 px period: 5 dashes of 5 px, with one dash straddling the corner
    // as two pieces (the corner is at 25 px, a dash start).
    CHECK(m.vertices.size() / 6 == 5);
}

TEST_CASE("markers produce outlines of the requested size") {
    for (MarkerKind kind : {MarkerKind::Square, MarkerKind::Triangle, MarkerKind::Circle,
                            MarkerKind::X, MarkerKind::Cross}) {
        Overlay o;
        o.marker(Space::Screen, {100, 100}, kind, 12, {255, 255, 0, 255});
        OverlayMesh m = o.build(camera());
        REQUIRE_FALSE(m.vertices.empty());
        Bounds b = boundsOf(m);
        CHECK(b.x0 >= 92.0f);  // 6 px half size plus the outline width
        CHECK(b.x1 <= 108.0f);
        CHECK(b.y0 >= 92.0f);
        CHECK(b.y1 <= 108.0f);
        CHECK(b.x1 - b.x0 >= 11.0f);
    }
}

TEST_CASE("filled markers add triangles inside the outline") {
    Overlay a, b;
    a.marker(Space::Screen, {50, 50}, MarkerKind::Square, 10, {255, 255, 255, 255}, false);
    b.marker(Space::Screen, {50, 50}, MarkerKind::Square, 10, {255, 255, 255, 255}, true);
    CHECK(b.build(camera()).vertices.size() == a.build(camera()).vertices.size() + 6);
}

TEST_CASE("selection window: solid blue for window, dashed green for crossing") {
    Overlay w, x;
    w.selectionWindow({10, 10}, {110, 60}, false);
    x.selectionWindow({110, 10}, {10, 60}, true);
    OverlayMesh mw = w.build(camera());
    OverlayMesh mx = x.build(camera());
    // Fill (2 triangles) first, then the border.
    CHECK(mw.vertices[0].b == 255);
    CHECK(mw.vertices[0].a < 100);
    CHECK(mx.vertices[0].g == 204);
    CHECK(mx.vertices[0].a < 100);
    // Window border: 4 solid quads after the 6 fill vertices.
    CHECK(mw.vertices.size() == 6 + 4 * 6);
    // Crossing border: dashed -> many more quads.
    CHECK(mx.vertices.size() > mw.vertices.size());
    Bounds b = boundsOf(mw);
    CHECK(b.x0 <= 10.0f);
    CHECK(b.x1 >= 110.0f);
}

TEST_CASE("rubber band is dashed") {
    Overlay o;
    o.rubberBand(Space::Screen, {0, 0}, {200, 0});
    CHECK(o.build(camera()).vertices.size() > 6);
}

TEST_CASE("fills triangulate concave polygons") {
    Overlay o;
    const Vec2 ring[] = {{0, 0}, {100, 0}, {100, 100}, {50, 40}, {0, 100}};
    o.fill(Space::Screen, ring, {10, 20, 30, 128});
    OverlayMesh m = o.build(camera());
    CHECK(m.vertices.size() == 9);  // 5 vertices -> 3 triangles
    CHECK(m.vertices[0].a == 128);
}

TEST_CASE("non-finite points are dropped") {
    Overlay o;
    o.line(Space::World, {0, 0}, {std::nan(""), 1}, OverlayStyle{});
    o.marker(Space::World, {INFINITY, 0}, MarkerKind::Circle, 10, {255, 255, 255, 255});
    CHECK(o.build(camera()).vertices.empty());
}

TEST_CASE("later shapes are emitted after earlier ones") {
    Overlay o;
    o.line(Space::Screen, {0, 0}, {10, 0}, OverlayStyle{{1, 2, 3, 255}, 1, 0, 0});
    o.line(Space::Screen, {0, 5}, {10, 5}, OverlayStyle{{9, 8, 7, 255}, 1, 0, 0});
    OverlayMesh m = o.build(camera());
    REQUIRE(m.vertices.size() == 12);
    CHECK(m.vertices[0].r == 1);
    CHECK(m.vertices[6].r == 9);
    o.clear();
    CHECK(o.empty());
}
