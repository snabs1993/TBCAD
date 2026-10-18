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
#include "tbcad/drafting/osnap.h"
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
        const double cmdMs = ms(t);
        core->updateFrame();
        const double run = ms(t);
        std::printf("  (%s: command %.0f ms, first frame %.0f ms)\n", label, cmdMs, run - cmdMs);
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
        const double undoDocMs = ms(t);
        core->updateFrame();
        const double undoMs = ms(t);
        t = std::chrono::steady_clock::now();
        const bool redone = core->redo();
        const double redoDocMs = ms(t);
        core->updateFrame();
        const double redoMs = ms(t);
        std::printf("  (%s: undo %.0f ms before the frame, redo %.0f ms before the frame)\n", label,
                    undoDocMs, redoDocMs);
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
    big.bench("CHPROP-C", {"CHPROP", "Color", "1"});
    big.core->submitText("-LAYER");
    big.core->submitText("Make");
    big.core->submitText("Other");
    big.core->submitText("");
    big.bench("CHPROP-L", {"CHPROP", "Layer", "Other"});
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

TEST_CASE("perf: MIRROR of everything alone") {
    Big big;
    big.bench("MIRROR", {"MIRROR", "0,0", "10,0", "N"});
}

TEST_CASE("perf: COPY of everything alone") {
    Big big;
    big.bench("COPY", {"COPY", "0,0", "5,5", ""});
}

TEST_CASE("perf: STRETCH of everything alone") {
    Big big;
    big.bench("STRETCH", {"STRETCH", "-100000,-100000", "100000,100000", "0,0", "5,0"});
}

TEST_CASE("perf: other whole-drawing commands") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Big big;
    std::printf("other commands on %d objects\n", 2 * big.n);
    big.bench("EXPLODE", {"EXPLODE"});
    big.bench("ARRAYRECT", {"ARRAYRECT", ""});
    big.bench("ALIGN", {"ALIGN", "0,0", "5,5", ""});
    big.bench("JOIN", {"JOIN"});
    big.bench("PEDIT", {"PEDIT", "Multiple", "Yes", "Join", "0.001", ""});
}

TEST_CASE("perf: layer and block commands on a big drawing") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Big big;
    std::printf("layer and block commands on %d objects\n", 2 * big.n);
    big.bench("LAYER-OFF", {"-LAYER", "Off", "0", ""});
    big.bench("LAYER-FRZ", {"-LAYER", "Freeze", "0", ""});
    big.bench("LAYER-LOCK", {"-LAYER", "Lock", "0", ""});
    big.bench("BLOCK", {"BLOCK", "Everything", "0,0"});
}

TEST_CASE("perf: selection on a big drawing") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Big big;
    std::printf("selection on %d objects\n", 2 * big.n);
    auto time = [&](const char* label, auto&& f) {
        auto t = std::chrono::steady_clock::now();
        const std::size_t n = f();
        std::printf("  %-22s %6.0f ms  (%zu picked)\n", label, ms(t), n);
    };
    const double w = 500 * 30.0, h = (big.n / 500) * 30.0;
    using S = cmd::IHost::SelectShape;
    time("window, everything", [&] {
        return big.core->selectShape(S::Window, {{-10, -10}, {w + 100, h + 100}}).value().size();
    });
    time("crossing, everything", [&] {
        return big.core->selectShape(S::Crossing, {{-10, -10}, {w + 100, h + 100}}).value().size();
    });
    time("crossing, a tenth", [&] {
        return big.core->selectShape(S::Crossing, {{0, 0}, {w / 3, h / 3}}).value().size();
    });
    time("fence across", [&] {
        return big.core->selectShape(S::Fence, {{-10, h / 2}, {w + 100, h / 2 + 7}}).value().size();
    });
    time("window polygon, all", [&] {
        return big.core
            ->selectShape(S::WPolygon,
                          {{-10, -10}, {w + 100, -10}, {w + 100, h + 100}, {-10, h + 100}})
            .value()
            .size();
    });
    time("crossing polygon, all", [&] {
        return big.core
            ->selectShape(S::CPolygon,
                          {{-10, -10}, {w + 100, -10}, {w + 100, h + 100}, {-10, h + 100}})
            .value()
            .size();
    });
    big.core->selectAll();
    for (const char* cmdName : {"SELECTSIMILAR", "QSELECT"}) {
        auto t = std::chrono::steady_clock::now();
        big.core->submitText(cmdName);
        std::printf("  %-22s %6.0f ms  (prompt: %s)\n", cmdName, ms(t),
                    big.core->promptText().c_str());
        big.core->submitText("");
        big.core->pressEscape();
    }
}

TEST_CASE("perf: QSELECT on a big drawing") {
    Big big;
    big.core->selectAll();
    for (const char* l : {"QSELECT", "Type", "Line", "", "", ""}) {
        auto t = std::chrono::steady_clock::now();
        big.core->submitText(l);
        std::printf("  %-8s %6.0f ms  -> %s\n", l, ms(t), big.core->promptText().c_str());
    }
    std::printf("  selected: %zu\n", big.core->selection().size());
}

TEST_CASE("perf: pointer movement with snaps on a big drawing") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Big big;
    std::printf("pointer on %d objects\n", 2 * big.n);
    auto run = [&](const char* label) {
        double worst = 0, total = 0;
        const int steps = 200;
        for (int i = 0; i < steps; ++i) {
            ui::PointerState p;
            p.screen = {100.0 + (i * 37) % 1100, 100.0 + (i * 53) % 700};
            auto t = std::chrono::steady_clock::now();
            big.core->pointerMoved(p);
            big.core->updateFrame();
            const double d = ms(t);
            worst = std::max(worst, d);
            total += d;
        }
        std::printf("  %-26s mean %.2f ms, worst %.2f ms per move+frame\n", label, total / steps,
                    worst);
    };
    run("idle (hover)");
    big.core->submitText("LINE");
    std::printf("  prompt: %s, active %d, osnap %s\n", big.core->promptText().c_str(),
                big.core->runner().active() ? 1 : 0,
                big.core->setting("drafting.osnap").value_or("?").c_str());
    run("LINE, first point");
    big.core->submitText("0,0");
    run("LINE, second point");
    big.core->pressEscape();
    // zoomed in on a few objects
    big.core->submitText("ZOOM");
    big.core->submitText("W");
    big.core->submitText("0,0");
    big.core->submitText("300,200");
    big.core->updateFrame();
    big.core->submitText("LINE");
    run("zoomed in, LINE");
    big.core->pressEscape();
}

TEST_CASE("perf: snap lookup with every mode on a big drawing") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Big big;
    std::printf("snap lookup on %d objects\n", 2 * big.n);
    const double w = 500 * 30.0, h = (big.n / 500) * 30.0;
    for (double aperture : {0.5, 5.0, 50.0, 500.0, 5000.0}) {
        drafting::SnapRequest req;
        req.apertureMm = aperture;
        req.modes = drafting::kAllSnapModes;
        req.base = Vec2{0, 0};
        double worst = 0, total = 0;
        std::size_t found = 0;
        const int steps = 100;
        for (int i = 0; i < steps; ++i) {
            req.cursor = {static_cast<double>((i * 7919) % static_cast<int>(w)),
                          static_cast<double>((i * 104729) % static_cast<int>(h))};
            auto t = std::chrono::steady_clock::now();
            const auto c =
                drafting::findSnapCandidates(big.core->document(), big.core->spatialIndex(), req);
            const double d = ms(t);
            found += c.size();
            worst = std::max(worst, d);
            total += d;
        }
        std::printf("  aperture %7.1f mm: mean %.3f ms, worst %.3f ms, %.1f candidates\n", aperture,
                    total / steps, worst, static_cast<double>(found) / steps);
    }
}
