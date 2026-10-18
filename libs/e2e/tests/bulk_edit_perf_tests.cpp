// How long the whole-drawing edits take on a drawing the size of a big site plan, with the scene
// and the spatial index attached, and how long their undo and redo take. Sizes: TBCAD_BENCH_N
// (default 100,000 polylines and 100,000 lines). Prints; the checks only guard the results.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
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

    Big() {
        dir = std::filesystem::temp_directory_path() / "tbcad_bulk_edit_perf";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core = std::make_unique<ui::AppCore>(registry,
                                             platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
        const char* env = std::getenv("TBCAD_BENCH_N");
        n = env ? std::atoi(env) : 100000;
        auto tx = core->document().begin("gen");
        for (int i = 0; i < n; ++i) {
            const double x = (i % 500) * 30.0, y = (i / 500) * 30.0;
            PolylineData p;
            for (int k = 0; k < 8; ++k)
                p.v.push_back({{x + k, y + k % 3}, 0, 0, 0});
            Entity e;
            e.data = std::move(p);
            (void)tx.add(std::move(e));
            (void)tx.add(Entity{{}, LineData{{x, y}, {x + 20, y + 10}}});
        }
        tx.commit();
        core->zoomExtents();
        core->updateFrame();
    }

    // Runs one command with the given answers on everything; prints the time of the command, its
    // undo and its redo, each including the next frame.
    void bench(const char* label, std::initializer_list<const char*> lines) {
        core->selectAll();
        const std::size_t before = core->document().modelSpaceCount();
        auto t = std::chrono::steady_clock::now();
        for (const char* l : lines)
            core->submitText(l);
        core->updateFrame();
        const double run = ms(t);
        const bool done = !core->runner().active() || core->promptText().rfind("Command", 0) == 0;
        if (!done) {
            // (a command left waiting at a prompt is ended with Enter)
            core->submitText("");
            std::printf("  (%s did not finish; prompt: %s)\n", label, core->promptText().c_str());
            const auto& h = core->history();
            for (std::size_t i = h.size() > 6 ? h.size() - 6 : 0; i < h.size(); ++i)
                std::printf("    | %s\n", h[i].c_str());
        }
        t = std::chrono::steady_clock::now();
        const bool undone = core->undo();
        core->updateFrame();
        const double undoMs = ms(t);
        t = std::chrono::steady_clock::now();
        const bool redone = core->redo();
        core->updateFrame();
        const double redoMs = ms(t);
        std::printf("%-8s run %6.0f ms   undo %6.0f ms   redo %6.0f ms   (%zu objects before, %zu "
                    "now)%s\n",
                    label, run, undoMs, redoMs, before, core->document().modelSpaceCount(),
                    done && undone && redone ? "" : "  [command, undo or redo did not run]");
        core->undo();
        core->updateFrame();
    }
};

}  // namespace

TEST_CASE("perf: whole-drawing edits with undo and redo") {
    Big big;
    std::printf("whole-drawing edits on %d objects\n", 2 * big.n);
    big.bench("MOVE", {"MOVE", "0,0", "5,5"});
    big.bench("COPY", {"COPY", "0,0", "5,5", ""});
    big.bench("ROTATE", {"ROTATE", "0,0", "30"});
    big.bench("SCALE", {"SCALE", "0,0", "1.5"});
    big.bench("MIRROR", {"MIRROR", "0,0", "10,0", "N"});
    big.bench("ERASE", {"ERASE"});
    big.bench("CHPROP-C", {"CHPROP", "Color", "1", ""});
    big.core->submitText("-LAYER");
    big.core->submitText("Make");
    big.core->submitText("Other");
    big.core->submitText("");
    big.bench("CHPROP-L", {"CHPROP", "Layer", "Other", ""});
}
