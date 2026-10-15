#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>

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
        for (EntityId id : core.document().modelSpace())
            (void)tx.erase(id);
        tx.commit();
    }
    std::printf("erase all (commit + scene sync): %.0f ms\n", ms(t));
}
