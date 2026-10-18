// Hatches at scale: 20,000 hatches (SOLID, ANSI31 and AR-CONC) on large areas. Draws the whole
// drawing, zooms into a window and pans, and watches time and memory. Size: TBCAD_BENCH_N hatches
// (default 20,000). Prints; the checks guard the results and generous limits.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
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

double rssMb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char line[256];
    double kb = 0;
    while (std::fgets(line, sizeof line, f))
        if (std::sscanf(line, "VmRSS: %lf kB", &kb) == 1)
            break;
    std::fclose(f);
    return kb / 1024.0;
}

HatchLoop rectLoop(double x, double y, double w, double h) {
    HatchLoop l;
    Vec2 p[4] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    for (int i = 0; i < 4; ++i)
        l.edges.push_back(HatchLineEdge{p[i], p[(i + 1) % 4]});
    return l;
}

}  // namespace

TEST_CASE("perf: 20,000 hatches - SOLID, ANSI31 and AR-CONC on large areas") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto dir = std::filesystem::temp_directory_path() /
                     ("tbcad_hatch_scale_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    cmd::Registry registry;
    registerAllCommands(registry);
    ui::AppCore core(registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1400, 900);
    const char* env = std::getenv("TBCAD_BENCH_N");
    const int n = env ? std::atoi(env) : 20000;

    const double base = rssMb();
    auto t = std::chrono::steady_clock::now();
    {
        auto tx = core.document().begin("hatches");
        for (int i = 0; i < n; ++i) {
            HatchData h;
            // 6 m x 4 m rooms on a grid, 200 across.
            h.loops.push_back(rectLoop((i % 200) * 7000.0, (i / 200) * 5000.0, 6000, 4000));
            switch (i % 3) {
                case 0:
                    h.solid = true;
                    break;
                case 1:
                    h.solid = false;
                    h.pattern = "ANSI31";
                    break;
                default:
                    h.solid = false;
                    h.pattern = "AR-CONC";
                    h.patternScale = 20;
                    break;
            }
            Entity e;
            e.data = Indirect<HatchData>(std::move(h));
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    std::printf("place %d hatches: %.0f ms\n", n, ms(t));
    t = std::chrono::steady_clock::now();
    core.zoomExtents();
    core.updateFrame();
    std::printf("first frame (everything in view): %.0f ms, memory +%.0f MB\n", ms(t),
                rssMb() - base);
    t = std::chrono::steady_clock::now();
    core.updateFrame();
    std::printf("next frame: %.0f ms\n", ms(t));

    // Zoom to a window of ~3x3 rooms: pattern lines are drawn for real.
    t = std::chrono::steady_clock::now();
    core.zoomToBox(BBox2{{0, 0}, {21000, 15000}});
    core.updateFrame();
    std::printf("zoom into 3x3 rooms: %.0f ms, memory +%.0f MB\n", ms(t), rssMb() - base);
    t = std::chrono::steady_clock::now();
    core.zoomToBox(BBox2{{0, 0}, {210000, 150000}});
    core.updateFrame();
    std::printf("zoom to 30x30 rooms: %.0f ms, memory +%.0f MB\n", ms(t), rssMb() - base);
    // Close in on one corner: the pattern lines are drawn for real; then walk along the rooms.
    t = std::chrono::steady_clock::now();
    core.zoomToBox(BBox2{{0, 0}, {1600, 1000}});
    core.updateFrame();
    std::printf("zoom into 1.6 m: %.0f ms, memory +%.0f MB\n", ms(t), rssMb() - base);
    double worst = 0;
    std::size_t strokeVerts = 0;
    for (int step = 0; step < 30; ++step) {
        t = std::chrono::steady_clock::now();
        core.zoomToBox(BBox2{{step * 7000.0, 0}, {step * 7000.0 + 1600, 1000}});
        core.updateFrame();
        worst = std::max(worst, ms(t));
        for (const auto& [k, tile] : core.scene().tiles())
            for (const auto& [bk, b] : tile.batches)
                if (bk.kind == render::BatchKind::Stroke)
                    strokeVerts = std::max(strokeVerts, b.verts.size());
    }
    std::printf(
        "walk along 30 rooms close in: worst step %.0f ms, memory +%.0f MB, biggest "
        "stroke batch %zu vertices\n",
        worst, rssMb() - base, strokeVerts);
    CHECK(strokeVerts > 1000);
    CHECK(worst < 3000);
    CHECK(rssMb() - base < 1500);
    t = std::chrono::steady_clock::now();
    core.zoomExtents();
    core.updateFrame();
    std::printf("back to everything: %.0f ms, memory +%.0f MB\n", ms(t), rssMb() - base);
    std::filesystem::remove_all(dir);
}
