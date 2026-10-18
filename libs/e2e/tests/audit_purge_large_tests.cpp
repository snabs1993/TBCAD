// AUDIT and PURGE on a drawing the size of a big site plan: they must finish in seconds, keep
// what is used, and undo in one step. Size: TBCAD_BENCH_N objects (default 100,000).
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>

#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;

namespace {

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

struct Big {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    int n = 0;
    int usedLayers = 0, unusedLayers = 0, unusedBlocks = 0;

    Big() {
        dir = std::filesystem::temp_directory_path() / "tbcad_audit_purge_large";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
        const char* env = std::getenv("TBCAD_BENCH_N");
        n = env ? std::atoi(env) : 100000;
        auto tx = core->document().begin("gen");
        usedLayers = 200;
        unusedLayers = 1500;
        unusedBlocks = 300;
        std::vector<LayerId> used;
        for (int i = 0; i < usedLayers; ++i)
            used.push_back(tx.addLayer({.name = "Used" + std::to_string(i)}).value());
        for (int i = 0; i < unusedLayers; ++i)
            (void)tx.addLayer({.name = "Spare" + std::to_string(i)});
        for (int i = 0; i < unusedBlocks; ++i) {
            auto b = tx.addBlock({.name = "Unused" + std::to_string(i)});
            Entity e;
            e.data = LineData{{0, 0}, {1, 1}};
            (void)tx.add(std::move(e), b.value());
        }
        for (int i = 0; i < n; ++i) {
            Entity e;
            e.common.layer = used[static_cast<std::size_t>(i % usedLayers)];
            e.data = LineData{{i * 1.0, 0}, {i * 1.0 + 5, 3}};
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
};

}  // namespace

TEST_CASE("perf: PURGE of a big drawing is fast, exact and one undo step") {
    Big big;
    const std::size_t entities = big.core->document().entityCount();
    const std::size_t layers = big.core->document().layers().size();
    const std::size_t blocks = big.core->document().blocks().size();
    auto t = std::chrono::steady_clock::now();
    big.core->submitText("PURGE");
    big.core->submitText("");
    big.core->submitText("Yes");
    const double purgeMs = ms(t);
    std::printf("PURGE on %zu objects, %zu layers, %zu blocks: %.0f ms\n", entities, layers, blocks,
                purgeMs);
    // (the standard unused layers of a new drawing go too: what stays is layer 0 and the used ones)
    CHECK(big.core->document().layers().size() == static_cast<std::size_t>(big.usedLayers) + 1);
    CHECK(big.core->document().blocks().size() ==
          blocks - static_cast<std::size_t>(big.unusedBlocks));
    CHECK(big.core->document().layerByName("Used7") != nullptr);
    CHECK(big.core->document().layerByName("Spare7") == nullptr);
    CHECK(big.core->document().entityCount() == entities - big.unusedBlocks);
    CHECK(purgeMs < 15000);
    CHECK(big.core->undo());
    CHECK(big.core->document().layers().size() == layers);
    CHECK(big.core->document().entityCount() == entities);
}

TEST_CASE("perf: AUDIT and RECOVER of a big drawing with broken objects") {
    Big big;
    {
        auto tx = big.core->document().begin("broken");
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (int i = 0; i < 50; ++i) {
            Entity z;
            z.data = LineData{{1, 1}, {1, 1}};  // zero length
            (void)tx.add(std::move(z));
        }
        // (a NaN circle is refused by validation, so only zero-length lines are broken here)
        (void)nan;
        tx.commit();
    }
    const std::size_t entities = big.core->document().entityCount();
    auto t = std::chrono::steady_clock::now();
    big.core->submitText("AUDIT");
    big.core->submitText("Yes");
    const double ms1 = ms(t);
    std::printf("AUDIT on %zu objects: %.0f ms\n", entities, ms1);
    CHECK(big.core->document().entityCount() == entities - 50);
    CHECK(ms1 < 10000);
    CHECK(big.core->undo());
    CHECK(big.core->document().entityCount() == entities);
    t = std::chrono::steady_clock::now();
    big.core->submitText("RECOVER");
    big.core->submitText("");
    const double ms2 = ms(t);
    std::printf("RECOVER on %zu objects: %.0f ms\n", entities, ms2);
    CHECK(big.core->document().entityCount() == entities - 50);
    CHECK(ms2 < 10000);
}
