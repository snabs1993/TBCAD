#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "tbcad/render/camera.h"
#include "tbcad/render/precision.h"

using namespace tbcad;
using namespace tbcad::render;

TEST_CASE("splitDouble keeps ~48 bits") {
    for (double v : {0.0, 1.0, -123456.789012345, 1e5 + 1e-9, 4e7 + 0.125}) {
        SplitFloat s = splitDouble(v);
        double back = static_cast<double>(s.hi) + static_cast<double>(s.lo);
        CHECK(std::abs(back - v) <= std::abs(v) * 1e-14 + 1e-300);
    }
}

TEST_CASE("float residual stays under 1e-3 px at 1e8 mm and 1000 px/mm") {
    const double tileSize = 1024;
    Camera2D cam;
    cam.center = {1e8 + 123.456789012, -1e8 + 7.654321098};
    cam.pixelsPerMm = 1000;
    cam.viewportW = 1920;
    cam.viewportH = 1080;

    std::mt19937_64 rng(1234);
    std::uniform_real_distribution<double> u(-1.0, 1.0);  // the view spans about +-1 mm
    double worst = 0, naiveWorst = 0;
    for (int i = 0; i < 20000; ++i) {
        // A world point near the camera centre, and the tile it belongs to.
        Vec2 p{cam.center.x + u(rng) * 0.96, cam.center.y + u(rng) * 0.54};
        Vec2 origin{std::floor(p.x / tileSize) * tileSize, std::floor(p.y / tileSize) * tileSize};
        GpuVertex v = packVertex(p, origin);
        TileOffset off = tileOffset(origin, cam.center);
        Vec2f rel = cameraRelative(v, off);

        const double exactX = (p.x - cam.center.x) * cam.pixelsPerMm;
        const double exactY = (p.y - cam.center.y) * cam.pixelsPerMm;
        const double errX = std::abs(static_cast<double>(rel.x) * cam.pixelsPerMm - exactX);
        const double errY = std::abs(static_cast<double>(rel.y) * cam.pixelsPerMm - exactY);
        worst = std::max({worst, errX, errY});

        // Sending world coordinates as plain floats (what the renderer must never do).
        const double naiveX = (static_cast<double>(static_cast<float>(p.x)) -
                               static_cast<double>(static_cast<float>(cam.center.x))) *
                              cam.pixelsPerMm;
        naiveWorst = std::max(naiveWorst, std::abs(naiveX - exactX));
    }
    CHECK(worst <= 1e-3);
    CHECK(naiveWorst > 1000);  // plain floats are hopeless here: ~8 mm steps at 1e8
}

TEST_CASE("the same holds on the far negative side and with tiles far from the camera") {
    const double tileSize = 16;
    const Vec2 center{-5e7 - 0.3333, 5e7 + 0.7777};
    // A vertex a whole screen away, in a different tile.
    const Vec2 p{center.x + 40.5, center.y - 22.25};
    Vec2 origin{std::floor(p.x / tileSize) * tileSize, std::floor(p.y / tileSize) * tileSize};
    Vec2f rel = cameraRelative(packVertex(p, origin), tileOffset(origin, center));
    CHECK(std::abs(static_cast<double>(rel.x) - 40.5) < 1e-5);
    CHECK(std::abs(static_cast<double>(rel.y) + 22.25) < 1e-5);
}

TEST_CASE("long distances inside a tile keep sub-micrometre precision") {
    // A vertex 5e5 mm from its tile origin (a long segment's far end).
    const Vec2 origin{0, 0};
    const Vec2 p{500000.123456789, 2.5};
    GpuVertex v = packVertex(p, origin);
    double x = static_cast<double>(v.hx) + static_cast<double>(v.lx);
    CHECK(std::abs(x - p.x) < 1e-8);
}
