#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

#include "tbcad/ui/app_core.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;

namespace {
double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
// Slowest and median of a series of timings.
struct Timings {
    std::vector<double> v{};
    void add(double x) { v.push_back(x); }
    double worst() const { return *std::max_element(v.begin(), v.end()); }
    double median() {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
};
}  // namespace

TEST_CASE("perf: pick, window select and pan on a drawing of 200,000 objects") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("pickpan") / "cfg", tempDir("pickpan") / "data"});
    core.setViewportSize(1400, 900);
    constexpr int kN = 200000;
    {
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> unit(0, 1);
        auto tx = core.document().begin("gen");
        for (int i = 0; i < kN; ++i) {
            const double x = (i % 500) * 400.0, y = (i / 500) * 400.0;  // a 200 m x 160 m sheet
            Entity e;
            switch (i % 3) {
                case 0:
                    e.data = LineData{{x, y}, {x + 150 + 100 * unit(rng), y + 120 * unit(rng)}};
                    break;
                case 1:
                    e.data = CircleData{{x + 60, y + 60}, 30 + 20 * unit(rng)};
                    break;
                default:
                    e.data = PolylineData{
                        {{{x, y}, 0}, {{x + 100, y}, 0.3}, {{x + 100, y + 80}, 0}}, false};
                    break;
            }
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    auto t = std::chrono::steady_clock::now();
    core.updateFrame();
    const double first = ms(t);
    core.zoomExtents();
    core.updateFrame();
    // Zoom in on a 20 m x 13 m part of the sheet: the usual working view.
    core.zoomToBox({{80000, 60000}, {100000, 73000}});
    core.updateFrame();

    Timings hover, click, window, crossing, pan, zoom;
    std::size_t hovered = 0, clicked = 0, windowed = 0, crossed = 0;
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> px(100, 1300), py(100, 800);
    for (int i = 0; i < 40; ++i) {
        PointerState p{{px(rng), py(rng)}, false, false};
        t = std::chrono::steady_clock::now();
        core.pointerMoved(p);
        hover.add(ms(t));
        hovered += core.hoveredEntity() ? 1 : 0;
        t = std::chrono::steady_clock::now();
        core.click(p);
        click.add(ms(t));
        clicked += core.selection().size();
        core.clearSelection();
    }
    for (int i = 0; i < 20; ++i) {
        const Vec2 a{px(rng) - 90, py(rng) - 90};
        PointerState p{a, false, false};
        t = std::chrono::steady_clock::now();
        core.dragSelect(a, a + Vec2{180, 180}, p);  // window
        window.add(ms(t));
        windowed += core.selection().size();
        core.clearSelection();
        t = std::chrono::steady_clock::now();
        core.dragSelect(a + Vec2{180, 180}, a, p);  // crossing
        crossing.add(ms(t));
        crossed += core.selection().size();
        core.clearSelection();
    }
    for (int i = 0; i < 40; ++i) {
        t = std::chrono::steady_clock::now();
        core.panPixels(i % 2 ? 60 : -60, 25);
        core.updateFrame();
        pan.add(ms(t));
    }
    for (int i = 0; i < 20; ++i) {
        t = std::chrono::steady_clock::now();
        core.zoomAtScreen({700, 450}, i % 2 ? 1.0 : -1.0);
        core.updateFrame();
        zoom.add(ms(t));
    }
    std::printf(
        "[pick/pan perf] %d objects, first frame %.0f ms\n  hover   median %.2f worst %.2f ms\n"
        "  click   median %.2f worst %.2f ms\n  window  median %.2f worst %.2f ms\n"
        "  crossing median %.2f worst %.2f ms\n  pan     median %.2f worst %.2f ms\n"
        "  zoom    median %.2f worst %.2f ms\n",
        kN, first, hover.median(), hover.worst(), click.median(), click.worst(), window.median(),
        window.worst(), crossing.median(), crossing.worst(), pan.median(), pan.worst(),
        zoom.median(), zoom.worst());
    std::printf("  hover hits %zu of 40, click selected %zu, window %zu, crossing %zu\n", hovered,
                clicked, windowed, crossed);
    // The view is full of objects, so the queries must have found some.
    CHECK(hovered > 0);
    CHECK(clicked > 0);
    CHECK(windowed > 0);
    CHECK(crossed >= windowed);
    // Generous bounds that also hold in an unoptimised build; the 16 ms frame budget is checked
    // when the build is optimised.
#ifdef NDEBUG
    const double budget = 16;
#else
    const double budget = 250;
#endif
    CHECK(hover.median() < budget);
    CHECK(click.median() < budget);
    CHECK(window.median() < budget);
    CHECK(crossing.median() < budget);
    CHECK(pan.median() < budget);
    CHECK(zoom.median() < budget);
}
