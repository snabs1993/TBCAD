// Dashed linetypes at scale: a drawing of TBCAD_BENCH_N (default 393,000) lines, polylines with
// arcs and circles, all in dashed linetypes. Opens it as a first frame, redraws after a zoom
// change and after LTSCALE, and compares with the same drawing in a continuous linetype.
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/export/plot.h"
#include "tbcad/ui/app_core.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace tbcad;

namespace {

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

struct Scene393k {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;

    Scene393k() {
        dir = std::filesystem::temp_directory_path() /
              ("tbcad_linetype_scale_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
    }
    ~Scene393k() { std::filesystem::remove_all(dir); }

    // `dashed` picks the linetype of every object.
    void fill(int n, bool dashed) {
        Document& doc = core->document();
        auto tx = doc.begin("fill");
        LinetypeId dash = kLinetypeContinuous, center = kLinetypeContinuous;
        if (dashed) {
            dash = tx.addLinetype({.name = "DASH393", .pattern = {12.7, -6.35}}).value();
            center =
                tx.addLinetype({.name = "CTR393", .pattern = {31.75, -6.35, 6.35, -6.35}}).value();
        }
        for (int i = 0; i < n; ++i) {
            const double x = (i % 700) * 120.0, y = (i / 700) * 120.0;
            Entity e;
            switch (i % 4) {
                case 0:
                    e.data = LineData{{x, y}, {x + 90, y + 40}};
                    break;
                case 1: {
                    PolylineData pl;
                    pl.v = {{{x, y}, 0},
                            {{x + 40, y}, 0.4},
                            {{x + 80, y + 30}, 0},
                            {{x + 90, y + 60}, 0}};
                    e.data = std::move(pl);
                    break;
                }
                case 2:
                    e.data = CircleData{{x + 40, y + 40}, 30};
                    break;
                default:
                    e.data = ArcData{{x + 40, y + 40}, 35, 0.3, 4.0};
                    break;
            }
            e.common.linetype = (i % 2) ? dash : center;
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
};

}  // namespace

TEST_CASE("perf: 393,000 dashed objects - first frame, zoom, LTSCALE") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* env = std::getenv("TBCAD_BENCH_N");
    const int n = env ? std::atoi(env) : 393000;
    double firstMs[2] = {0, 0};
    for (int dashed = 0; dashed < 2; ++dashed) {
        Scene393k s;
        auto t = std::chrono::steady_clock::now();
        s.fill(n, dashed != 0);
        const double fillMs = ms(t);
        t = std::chrono::steady_clock::now();
        s.core->zoomExtents();
        s.core->updateFrame();
        firstMs[dashed] = ms(t);
        std::printf("%s: fill %.0f ms, first frame %.0f ms\n", dashed ? "dashed" : "continuous",
                    fillMs, firstMs[dashed]);
        t = std::chrono::steady_clock::now();
        s.core->zoomToBox(BBox2{{0, 0}, {8000, 5000}});
        s.core->updateFrame();
        std::printf("  zoom into a window: %.0f ms\n", ms(t));
        auto maxPeriod = [&] {
            double m = 0;
            for (const auto& [k, tile] : s.core->scene().tiles())
                for (const auto& [bk, b] : tile.batches)
                    m = std::max(m, s.core->scene().pattern(bk.pattern).period());
            return m;
        };
        const double before = maxPeriod();
        t = std::chrono::steady_clock::now();
        s.core->submitText("LTSCALE");
        s.core->submitText("2");
        s.core->zoomExtents();
        s.core->updateFrame();
        std::printf("  LTSCALE 2 and redraw everything: %.0f ms\n", ms(t));
        if (dashed)
            CHECK(maxPeriod() == doctest::Approx(before * 2));
    }
    CHECK(firstMs[1] < firstMs[0] * 3 + 2000);
}

namespace {
// Is the point `t` mm along a dashed paper path inside a dash? `dash` alternates on/off lengths.
bool onAt(const std::vector<double>& dash, double t) {
    double period = 0;
    for (double d : dash)
        period += d;
    t = std::fmod(t, period);
    double acc = 0;
    for (std::size_t i = 0; i < dash.size(); ++i) {
        if (t < acc + dash[i])
            return i % 2 == 0;
        acc += dash[i];
    }
    return false;
}
}  // namespace

TEST_CASE("the plot draws the same dashes as the screen, at plot scale 1:N with MSLTSCALE") {
    Scene393k s;
    Document& doc = s.core->document();
    {
        auto tx = doc.begin("lt");
        LinetypeId lt =
            tx.addLinetype({.name = "MIXED", .pattern = {12, -6, 0, -6, 3, -4}}).value();
        Entity e;
        e.data = LineData{{0, 0}, {2000, 0}};
        e.common.linetype = lt;
        e.common.linetypeScale = 1.5f;
        (void)tx.add(std::move(e));
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.ltScale = 2;
                      h.custom[kAnnoScaleKey] = "50";
                  }).ok());
        tx.commit();
    }
    s.core->zoomExtents();
    s.core->updateFrame();
    const render::DashPattern* screen = nullptr;
    for (const auto& [k, tile] : s.core->scene().tiles())
        for (const auto& [bk, b] : tile.batches)
            if (bk.pattern != 0)
                screen = &s.core->scene().pattern(bk.pattern);
    REQUIRE(screen);
    // 12+6+0+6+3+4 = 31, times object 1.5, LTSCALE 2 and the annotation scale 50.
    CHECK(screen->period() == doctest::Approx(31 * 1.5 * 2 * 50));

    exportlib::PlotSettings ps;
    ps.fitToPaper = false;
    ps.scaleN = 50;
    ps.paperName = "A0";
    auto sheet = exportlib::buildSheet(doc, ps);
    REQUIRE(sheet.ok());
    const exportlib::PlotPath* path = nullptr;
    for (const auto& prim : sheet.value().prims)
        if (const auto* p = std::get_if<exportlib::PlotPath>(&prim))
            path = p;
    REQUIRE(path);
    REQUIRE_FALSE(path->style.dashMm.empty());
    double plotPeriod = 0;
    for (double d : path->style.dashMm)
        plotPeriod += d;
    // The same period on paper (1:50) as on screen, dots included.
    CHECK(plotPeriod * 50 == doctest::Approx(screen->period()));
    // And the same pattern of dashes and gaps along the line, away from the dots themselves.
    int compared = 0;
    for (double d = 0; d < 1900; d += 0.37) {
        const double inCycle = std::fmod(d, screen->period());
        const double dotAt = (12 + 6) * 1.5 * 2 * 50;
        if (std::abs(inCycle - dotAt) < 3 * 50 * ps.hairlineMm + 1)
            continue;
        CHECK(screen->isDash(d, 1e-9) == onAt(path->style.dashMm, d / 50));
        ++compared;
    }
    CHECK(compared > 4000);
}
