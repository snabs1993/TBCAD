#include <doctest/doctest.h>

#include <cmath>

#include "tbcad/print/print.h"

using namespace tbcad;
using namespace tbcad::print;
using namespace tbcad::exportlib;

namespace {

struct Recorder final : Canvas {
    struct Line {
        std::vector<Vec2> pts;
        bool closed;
        double width;
        std::vector<double> dash;
        Rgba color;
    };
    std::vector<Line> lines;
    int fills = 0, dots = 0, clipsPushed = 0, clipsPopped = 0;
    BBox2 lastClip;
    void polyline(const std::vector<Vec2>& px, bool closed, Rgba color, double w,
                  const std::vector<double>& dash) override {
        lines.push_back({px, closed, w, dash, color});
    }
    void fill(const std::vector<std::vector<Vec2>>&, Rgba) override { ++fills; }
    void dot(Vec2, Rgba, double) override { ++dots; }
    void pushClip(const BBox2& r) override {
        ++clipsPushed;
        lastClip = r;
    }
    void popClip() override { ++clipsPopped; }
};

// A 100 x 50 mm page printed at 254 dpi, so that 1 mm = 10 px.
DeviceMetrics metrics() {
    DeviceMetrics m;
    m.dpiX = m.dpiY = 254;
    m.pageWidthPx = 1000;
    m.pageHeightPx = 500;
    return m;
}

PlotSheet sheet() {
    PlotSheet s;
    s.layout.pageWidthMm = 100;
    s.layout.pageHeightMm = 50;
    return s;
}

}  // namespace

TEST_CASE("paper millimetres map to device pixels with y flipped") {
    CHECK(toDevice({0, 0}, metrics()).x == doctest::Approx(0));
    CHECK(toDevice({0, 0}, metrics()).y == doctest::Approx(500));
    CHECK(toDevice({100, 50}, metrics()).x == doctest::Approx(1000));
    CHECK(toDevice({100, 50}, metrics()).y == doctest::Approx(0));
    DeviceMetrics m = metrics();
    m.offsetXPx = 20;  // the printable area starts 2 mm in
    m.offsetYPx = 30;
    CHECK(toDevice({10, 10}, m).x == doctest::Approx(80));
    CHECK(toDevice({10, 10}, m).y == doctest::Approx(370));
}

TEST_CASE("paths, widths and dashes reach the canvas in pixels") {
    PlotSheet s = sheet();
    PlotPath p;
    p.pts = {{0, 0}, {10, 0}};
    p.style.widthMm = 0.5;
    p.style.dashMm = {2, 1};
    p.style.color = {10, 20, 30, 255};
    s.prims.push_back(p);
    PlotPath dot;  // fewer than two points: ignored
    dot.pts = {{1, 1}};
    s.prims.push_back(dot);
    Recorder r;
    drawSheet(s, metrics(), r);
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].pts[1].x == doctest::Approx(100));
    CHECK(r.lines[0].width == doctest::Approx(5));
    CHECK(r.lines[0].dash == std::vector<double>{20, 10});
    CHECK(r.lines[0].color == Rgba{10, 20, 30, 255});
    CHECK_FALSE(r.lines[0].closed);
}

TEST_CASE("hairlines are at least one pixel wide") {
    PlotSheet s = sheet();
    PlotPath p;
    p.pts = {{0, 0}, {10, 0}};
    p.style.widthMm = 0.001;
    s.prims.push_back(p);
    Recorder r;
    drawSheet(s, metrics(), r);
    CHECK(r.lines[0].width == doctest::Approx(1));
}

TEST_CASE("a full circle becomes a closed ring of points on the circle") {
    PlotSheet s = sheet();
    PlotArc a;
    a.center = {50, 25};
    a.radius = 10;
    a.full = true;
    s.prims.push_back(a);
    Recorder r;
    drawSheet(s, metrics(), r);
    REQUIRE(r.lines.size() == 1);
    CHECK(r.lines[0].closed);
    CHECK(r.lines[0].pts.size() > 30);
    for (const Vec2& p : r.lines[0].pts)
        CHECK(std::hypot(p.x - 500, p.y - 250) == doctest::Approx(100).epsilon(0.01));
}

TEST_CASE("an arc runs counter-clockwise from start to end, also across zero") {
    PlotSheet s = sheet();
    PlotArc a;
    a.center = {50, 25};
    a.radius = 10;
    a.start = 3 * kPi / 2;  // 270 degrees to 90 degrees: the right half
    a.end = kPi / 2;
    s.prims.push_back(a);
    Recorder r;
    drawSheet(s, metrics(), r);
    REQUIRE(r.lines.size() == 1);
    CHECK_FALSE(r.lines[0].closed);
    const auto& pts = r.lines[0].pts;
    for (const Vec2& p : pts)
        CHECK(p.x >= 500 - 1e-6);
    CHECK(pts.front().y > pts.back().y);  // starts at the bottom (larger pixel y)
}

TEST_CASE("fills, dots and clip regions are passed on; unbalanced clips are closed") {
    PlotSheet s = sheet();
    PlotFill f;
    f.rings = {{{0, 0}, {10, 0}, {10, 10}}, {{1, 1}}};  // the second ring is too short
    s.prims.push_back(f);
    PlotDots d;
    d.pts = {{5, 5}, {6, 6}};
    s.prims.push_back(d);
    PlotClip c;
    c.rect.expand({10, 10});
    c.rect.expand({20, 20});
    c.begin = true;
    s.prims.push_back(c);
    PlotPath p;
    p.pts = {{0, 0}, {50, 0}};
    s.prims.push_back(p);  // the clip is never ended
    Recorder r;
    drawSheet(s, metrics(), r);
    CHECK(r.fills == 1);
    CHECK(r.dots == 2);
    CHECK(r.clipsPushed == 1);
    CHECK(r.clipsPopped == 1);
    CHECK(r.lastClip.min.x == doctest::Approx(100));
    CHECK(r.lastClip.min.y == doctest::Approx(300));  // 20 mm up from the bottom edge
    CHECK(r.lastClip.max.y == doctest::Approx(400));
}

TEST_CASE("the stub backend is honest about not printing") {
#ifdef _WIN32
    CHECK(available());
#else
    CHECK_FALSE(available());
    CHECK(listPrinters().empty());
    auto r = printSheet(sheet(), PrintJob{});
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("Windows") != std::string::npos);
#endif
}
