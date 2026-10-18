// How long the whole-drawing edits take on a drawing the size of a big site plan, with the scene
// and the spatial index attached, and how long their undo and redo take. Sizes: TBCAD_BENCH_N
// (default 100,000 polylines and 100,000 lines). Prints; the checks only guard the results.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"
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

struct Big {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    int n = 0;

    Big() {
        dir = std::filesystem::temp_directory_path() /
              ("tbcad_bulk_edit_perf_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
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
        // What the first frame left unbuilt is built in the frames after it.
        auto ts = std::chrono::steady_clock::now();
        int frames = 0;
        while (core->sceneBuilding() && frames < 10000) {
            core->updateFrame();
            ++frames;
        }
        const double settleMs = ms(ts);
        std::printf("  %s: %d more frames to build the rest, %.0f ms\n", label, frames, settleMs);
        CHECK(core->scene().entityCount() == core->document().modelSpaceCount());
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
        std::printf(
            "%-8s run %6.0f ms   undo %6.0f ms   redo %6.0f ms   (%zu objects before, %zu "
            "now)%s\n",
            label, run, undoMs, redoMs, before, core->document().modelSpaceCount(),
            done && undone && redone ? "" : "  [command, undo or redo did not run]");
        core->undo();
        core->updateFrame();
    }
};

}  // namespace

TEST_CASE("perf: whole-drawing edits with undo and redo") {
    // Unbuffered, so that a crash still leaves the lines printed so far (they show where it was).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Big big;
    std::printf("whole-drawing edits on %d objects\n", 2 * big.n);
    big.bench("MOVE", {"MOVE", "0,0", "5,5"});
    big.bench("COPY", {"COPY", "0,0", "5,5", ""});
    big.bench("ROTATE", {"ROTATE", "0,0", "30"});
    big.bench("SCALE", {"SCALE", "0,0", "1.5"});
    big.bench("MIRROR", {"MIRROR", "0,0", "10,0", "N"});
    big.bench("STRETCH", {"STRETCH", "-100000,-100000", "100000,100000", "0,0", "5,0"});
    big.bench("ERASE", {"ERASE"});
    big.bench("CHPROP-C", {"CHPROP", "Color", "1", ""});
    big.core->submitText("-LAYER");
    big.core->submitText("Make");
    big.core->submitText("Other");
    big.core->submitText("");
    big.bench("CHPROP-L", {"CHPROP", "Layer", "Other", ""});
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

TEST_CASE("perf: memory of the undo stack after 50 whole-drawing edits") {
    Big big;
    const double base = rssMb();
    std::printf("undo memory: %zu objects, %.0f MB before any edit, limit %.0f MB\n",
                big.core->document().modelSpaceCount(), base,
                static_cast<double>(big.core->undoStack().limitBytes()) / 1048576.0);
    for (int i = 1; i <= 50; ++i) {
        big.core->selectAll();
        for (const char* l : {"MOVE", "0,0", "1,1"})
            big.core->submitText(l);
        if (i % 10 == 0 || i == 1)
            std::printf("  after %2d edits: %zu steps kept, stack says %.0f MB, process %.0f MB\n",
                        i, big.core->undoStack().undoCount(),
                        static_cast<double>(big.core->undoStack().usedBytes()) / 1048576.0,
                        rssMb());
    }
    CHECK(big.core->undoStack().usedBytes() <= big.core->undoStack().limitBytes());
}

TEST_CASE("perf: save and open of a big drawing keep every object") {
    Big big;
    const std::size_t count = big.core->document().modelSpaceCount();
    const auto path = big.dir / "big.tbcad";
    auto t = std::chrono::steady_clock::now();
    auto saved = big.core->saveAs(path);
    const double saveMs = ms(t);
    REQUIRE(saved.ok());
    {
        t = std::chrono::steady_clock::now();
        auto mem = io::writeTbcadToMemory(big.core->document());
        const double encodeMs = ms(t);
        REQUIRE(mem.ok());
        io::WriteOptions raw;
        raw.compress = false;
        t = std::chrono::steady_clock::now();
        auto memRaw = io::writeTbcadToMemory(big.core->document(), raw);
        const double rawMs = ms(t);
        io::IoReport rep;
        t = std::chrono::steady_clock::now();
        auto back = io::readTbcad(std::span<const std::uint8_t>(mem.value()), rep);
        const double decodeMs = ms(t);
        std::printf(
            "  encode %.0f ms (%.1f MB), without deflate %.0f ms (%.1f MB), decode %.0f ms\n",
            encodeMs, static_cast<double>(mem.value().size()) / 1e6, rawMs,
            static_cast<double>(memRaw.value().size()) / 1e6, decodeMs);
    }
    ui::AppCore fresh(big.registry, platform::AppPaths{big.dir / "cfg2", big.dir / "data2"});
    fresh.setViewportSize(1400, 900);
    t = std::chrono::steady_clock::now();
    auto opened = fresh.openFile(path);
    fresh.updateFrame();
    const double openMs = ms(t);
    REQUIRE(opened.ok());
    std::printf("save %zu objects: %.0f ms (%.1f MB)   open: %.0f ms\n", count, saveMs,
                static_cast<double>(std::filesystem::file_size(path)) / 1e6, openMs);
    CHECK(fresh.document().modelSpaceCount() == count);
    CHECK(canonicalDump(fresh.document()) == canonicalDump(big.core->document()));
}

TEST_CASE("perf: ERASE of everything alone") {
    Big big;
    big.bench("ERASE", {"ERASE"});
}

TEST_CASE("perf: MOVE of everything alone") {
    Big big;
    big.bench("MOVE", {"MOVE", "0,0", "5,5"});
}
