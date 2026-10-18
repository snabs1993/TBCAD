#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "tbcad/blocks/tbcad_blocks.h"
#include "tbcad/layout/layout.h"
#include "tbcad/ui/app_core.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;

namespace {
double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
}  // namespace

TEST_CASE("perf: select all on a drawing of many block inserts") {
    blocks::registerInsertDisplay();
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("selall") / "cfg", tempDir("selall") / "data"});
    core.setViewportSize(1400, 900);
    const int nBlocks = 200, nInserts = 100000, nLines = 100000;
    {
        auto tx = core.document().begin("gen");
        std::vector<BlockId> blocksIds;
        for (int b = 0; b < nBlocks; ++b) {
            BlockId id = tx.addBlock(BlockDef{.name = "B" + std::to_string(b)}).value();
            for (int k = 0; k < 6; ++k)
                (void)tx.add(Entity{{}, LineData{{k * 3.0, 0}, {k * 3.0 + 2, 5.0 + b % 7}}}, id);
            blocksIds.push_back(id);
        }
        for (int i = 0; i < nInserts; ++i) {
            Entity e;
            e.data = Indirect<InsertData>(InsertData{
                blocksIds[i % nBlocks], {(i % 400) * 50.0, (i / 400) * 40.0}, {1, 1}, 0});
            (void)tx.add(std::move(e));
        }
        for (int i = 0; i < nLines; ++i)
            (void)tx.add(Entity{{},
                                LineData{{(i % 500) * 30.0, (i / 500) * 30.0},
                                         {(i % 500) * 30.0 + 20, (i / 500) * 30.0 + 10}}});
        tx.commit();
    }
    auto t = std::chrono::steady_clock::now();
    core.updateFrame();
    std::printf("updateFrame after gen: %.0f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    core.zoomExtents();
    core.updateFrame();
    std::printf("zoom extents: %.0f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    core.selectAll();
    std::printf("selectAll: %.0f ms (%zu)\n", ms(t), core.selection().size());
    t = std::chrono::steady_clock::now();
    const auto& geo = core.scene().selectedGeometry();
    std::printf("selectedGeometry: %.0f ms (%zu tiles)\n", ms(t), geo.tiles.size());
    t = std::chrono::steady_clock::now();
    core.updateFrame();
    std::printf("updateFrame with selection: %.0f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    render::Overlay overlay;
    core.buildOverlay(overlay);
    std::printf("buildOverlay with selection: %.0f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    {
        auto tx = core.document().begin("erase all");
        const auto ids = core.document().modelSpace();
        std::printf("  modelSpace: %.0f ms\n", ms(t));
        auto t1 = std::chrono::steady_clock::now();
        for (EntityId id : ids)
            (void)tx.erase(id);
        std::printf("  erase loop: %.0f ms\n", ms(t1));
        t1 = std::chrono::steady_clock::now();
        tx.commit();
        std::printf("  commit (observers): %.0f ms\n", ms(t1));
    }
    std::printf("erase all (commit + scene sync): %.0f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    core.updateFrame();
    std::printf("updateFrame after erase: %.0f ms\n", ms(t));
}

TEST_CASE("perf: erase all of polylines and texts") {
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("eraseheavy") / "cfg", tempDir("eraseheavy") / "data"});
    core.setViewportSize(1400, 900);
    const char* env = std::getenv("TBCAD_BENCH_N");
    const int n = env ? std::atoi(env) : 100000;
    {
        auto tx = core.document().begin("gen");
        for (int i = 0; i < n; ++i) {
            PolylineData p;
            for (int k = 0; k < 30; ++k)
                p.v.push_back({{(i % 500) * 30.0 + k, (i / 500) * 30.0 + k % 3}, 0, 0, 0});
            Entity e;
            e.data = std::move(p);
            (void)tx.add(std::move(e));
            TextData t;
            t.position = {(i % 500) * 30.0, (i / 500) * 30.0};
            t.utf8 = "label number " + std::to_string(i) + " with a longer text body";
            Entity te;
            te.data = std::move(t);
            (void)tx.add(std::move(te));
        }
        tx.commit();
    }
    core.updateFrame();
    auto t = std::chrono::steady_clock::now();
    {
        auto tx = core.document().begin("erase all");
        const auto ids = core.document().modelSpace();
        auto t1 = std::chrono::steady_clock::now();
        for (EntityId id : ids)
            (void)tx.erase(id);
        std::printf("  heavy erase loop: %.0f ms\n", ms(t1));
        t1 = std::chrono::steady_clock::now();
        tx.commit();
        std::printf("  heavy commit: %.0f ms\n", ms(t1));
    }
    std::printf("heavy erase all (%d entities): %.0f ms\n", 2 * n, ms(t));
    t = std::chrono::steady_clock::now();
    core.updateFrame();
    std::printf("heavy updateFrame after erase: %.0f ms\n", ms(t));
}

namespace {
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
}  // namespace

TEST_CASE("perf: memory of a large mixed drawing") {
    {
        // A bare document (no scene, no index observers) and its undo set, for comparison.
        const char* env0 = std::getenv("TBCAD_BENCH_N");
        const int m = env0 ? std::atoi(env0) : 100000;
        const double b0 = rssMb();
        Document bare;
        {
            auto tx = bare.begin("gen");
            for (int i = 0; i < m; ++i) {
                const double x = (i % 500) * 30.0, y = (i / 500) * 30.0;
                (void)tx.add(Entity{{}, LineData{{x, y}, {x + 20, y + 10}}});
                (void)tx.add(Entity{{}, CircleData{{x, y}, 3}});
                PolylineData p;
                for (int k = 0; k < 8; ++k)
                    p.v.push_back({{x + k, y + k % 3}, 0, 0, 0});
                Entity e;
                e.data = std::move(p);
                (void)tx.add(std::move(e));
                TextData t;
                t.position = {x, y};
                t.utf8 = "label " + std::to_string(i);
                Entity te;
                te.data = std::move(t);
                (void)tx.add(std::move(te));
            }
            const double beforeCommit = rssMb();
            tx.commit();
            std::printf(
                "bare document, %d entities: %.0f MB before commit, %.0f MB after (%.0f B "
                "each)\n",
                4 * m, beforeCommit - b0, rssMb() - b0, (rssMb() - b0) * 1048576 / (4.0 * m));
        }
    }
    auto registry = makeRegistry();
    AppCore core(*registry,
                 platform::AppPaths{tempDir("memmix") / "cfg", tempDir("memmix") / "data"});
    core.setViewportSize(1400, 900);
    const char* env = std::getenv("TBCAD_BENCH_N");
    const int n = env ? std::atoi(env) : 100000;  // n lines + n polylines + n texts + n circles
    const double base = rssMb();
    {
        auto tx = core.document().begin("gen");
        for (int i = 0; i < n; ++i) {
            const double x = (i % 500) * 30.0, y = (i / 500) * 30.0;
            (void)tx.add(Entity{{}, LineData{{x, y}, {x + 20, y + 10}}});
            (void)tx.add(Entity{{}, CircleData{{x, y}, 3}});
            PolylineData p;
            for (int k = 0; k < 8; ++k)
                p.v.push_back({{x + k, y + k % 3}, 0, 0, 0});
            Entity e;
            e.data = std::move(p);
            (void)tx.add(std::move(e));
            TextData t;
            t.position = {x, y};
            t.utf8 = "label " + std::to_string(i);
            Entity te;
            te.data = std::move(t);
            (void)tx.add(std::move(te));
        }
        tx.commit();
    }
    const double afterDoc = rssMb();
    core.updateFrame();
    const double afterScene = rssMb();
    core.zoomExtents();
    core.updateFrame();
    const double afterFrame = rssMb();
    std::printf(
        "memory for %d entities: document+index+undo %.0f MB (%.0f B each), scene %.0f MB "
        "(%.0f B each), after zoom %.0f MB\n",
        4 * n, afterDoc - base, (afterDoc - base) * 1048576 / (4.0 * n), afterScene - afterDoc,
        (afterScene - afterDoc) * 1048576 / (4.0 * n), afterFrame - afterScene);
}

TEST_CASE("perf: memory after opening a large DXF") {
    const char* env = std::getenv("TBCAD_BENCH_N");
    const int n = env ? std::atoi(env) : 100000;
    const auto dir = tempDir("memdxf");
    const auto path = dir / "big.dxf";
    {
        std::ofstream o(path);
        o << "0\nSECTION\n2\nENTITIES\n";
        for (int i = 0; i < n; ++i) {
            const double x = (i % 500) * 30.0, y = (i / 500) * 30.0;
            o << "0\nLINE\n8\n0\n10\n"
              << x << "\n20\n"
              << y << "\n11\n"
              << x + 20 << "\n21\n"
              << y + 10 << "\n";
            o << "0\nCIRCLE\n8\n0\n10\n" << x << "\n20\n" << y << "\n40\n3\n";
            o << "0\nLWPOLYLINE\n8\n0\n90\n8\n70\n0\n";
            for (int k = 0; k < 8; ++k)
                o << "10\n" << x + k << "\n20\n" << y + k % 3 << "\n";
            o << "0\nTEXT\n8\n0\n10\n" << x << "\n20\n" << y << "\n40\n2.5\n1\nlabel " << i << "\n";
        }
        o << "0\nENDSEC\n0\nEOF\n";
    }
    auto registry = makeRegistry();
    const double base = rssMb();
    AppCore core(*registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1400, 900);
    auto t = std::chrono::steady_clock::now();
    REQUIRE(core.openFile(path).ok());
    const double afterOpen = rssMb();
    const double openMs = ms(t);
    core.zoomExtents();
    core.updateFrame();
    const double afterFrame = rssMb();
    std::printf(
        "open %d entities: %.0f ms, RSS +%.0f MB (%.0f B each), after first frame +%.0f MB\n",
        4 * n, openMs, afterOpen - base, (afterOpen - base) * 1048576 / (4.0 * n),
        afterFrame - base);
}
