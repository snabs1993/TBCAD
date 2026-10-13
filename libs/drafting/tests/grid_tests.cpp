#include <doctest/doctest.h>

#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

TEST_CASE("grid spacing skips levels so lines stay at least 8 px apart") {
    CHECK(effectiveGridSpacing(10, 1.0) == doctest::Approx(10));   // 10 px
    CHECK(effectiveGridSpacing(10, 0.8) == doctest::Approx(10));   // exactly 8 px
    CHECK(effectiveGridSpacing(10, 0.5) == doctest::Approx(50));   // 5 px -> x5 = 25 px
    CHECK(effectiveGridSpacing(10, 0.1) == doctest::Approx(100));  // 1 px -> 10 px after x10
    CHECK(effectiveGridSpacing(10, 0.01) == doctest::Approx(1000));
    CHECK(effectiveGridSpacing(10, 0.001) * 0.001 >= 8);
    CHECK(effectiveGridSpacing(0, 1) == 0);
    CHECK(effectiveGridSpacing(-5, 1) == 0);
    for (double ppm : {0.003, 0.02, 0.07, 0.3, 0.79}) {
        double s = effectiveGridSpacing(10, ppm);
        CHECK(s * ppm >= 8 - 1e-9);
        // Never skips more than needed: one level finer would be below 8 px.
        CHECK(s / 10 * ppm < 8);
    }
}

TEST_CASE("the grid overlay is built for the visible area and stays small when zoomed out") {
    render::Camera2D cam;
    cam.viewportW = 1000;
    cam.viewportH = 800;
    cam.center = {500, 400};
    cam.pixelsPerMm = 1.0;
    render::Overlay ov;
    GridStyle st;
    st.spacingMm = 10;
    buildGrid(ov, cam, st);
    CHECK_FALSE(ov.empty());
    CHECK_FALSE(ov.build(cam).vertices.empty());

    cam.pixelsPerMm = 1e-4;  // whole city: the spacing skips to keep line count small
    render::Overlay far;
    buildGrid(far, cam, st);
    CHECK(far.build(cam).vertices.size() < 100000);

    st.spacingMm = 0;
    render::Overlay none;
    buildGrid(none, cam, st);
    CHECK(none.empty());

    st.spacingMm = 10;
    st.lines = true;
    cam.pixelsPerMm = 1.0;
    render::Overlay lines;
    buildGrid(lines, cam, st);
    render::Overlay dots;
    st.lines = false;
    buildGrid(dots, cam, st);
    CHECK_FALSE(lines.empty());
    CHECK_FALSE(dots.empty());
}

TEST_CASE("every snap mode has a marker and unknown names are refused") {
    render::Camera2D cam;
    cam.viewportW = 400;
    cam.viewportH = 300;
    for (int i = 0; i < kSnapModeCount; ++i) {
        render::Overlay ov;
        auto mode = static_cast<SnapMode>(i);
        CHECK(drawSnapMarker(ov, cam, {0, 0}, snapModeName(mode)));
        CHECK_FALSE(ov.empty());
    }
    render::Overlay ov;
    CHECK_FALSE(drawSnapMarker(ov, cam, {0, 0}, "end"));  // prefixes are not marker names
    CHECK_FALSE(drawSnapMarker(ov, cam, {0, 0}, ""));
    CHECK(ov.empty());
}
