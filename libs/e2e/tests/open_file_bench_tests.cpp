// Opens the drawing named by TBCAD_BENCH_FILE (skipped without it) and reports the time and
// memory of opening it, the first frames and a selection of everything: for profiling one slow
// drawing.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>

#include "tbcad/cmd/register_all.h"
#include "tbcad/ui/app_core.h"

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

}  // namespace

TEST_CASE("bench: open one drawing (TBCAD_BENCH_FILE)") {
    const char* file = std::getenv("TBCAD_BENCH_FILE");
    if (!file || !*file)
        return;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto dir = std::filesystem::temp_directory_path() / "tbcad_open_bench";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    cmd::Registry registry;
    registerAllCommands(registry);
    ui::AppCore core(registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1400, 900);
    const double base = rssMb();
    auto t = std::chrono::steady_clock::now();
    REQUIRE(core.openFile(file).ok());
    std::printf("open: %.0f ms, memory +%.0f MB\n", ms(t), rssMb() - base);
    for (const auto& [name, lap] : core.openTimings())
        std::printf("  %-36s %8.1f ms\n", name.c_str(), lap);
    t = std::chrono::steady_clock::now();
    core.zoomExtents();
    core.updateFrame();
    std::printf("first frame: %.0f ms, memory +%.0f MB\n", ms(t), rssMb() - base);
    for (int i = 0; i < 3; ++i) {
        t = std::chrono::steady_clock::now();
        core.updateFrame();
        std::printf("frame: %.1f ms\n", ms(t));
    }
    auto run = [&](const char* label, std::initializer_list<const char*> lines) {
        t = std::chrono::steady_clock::now();
        for (const char* l : lines)
            core.submitText(l);
        const double edit = ms(t);
        core.updateFrame();
        std::printf("%-12s %8.0f ms (frame %.0f ms), memory +%.0f MB\n", label, ms(t),
                    ms(t) - edit, rssMb() - base);
    };
    auto undoRedo = [&] {
        run("  undo", {"U"});
        run("  redo", {"REDO"});
    };
    run("move all", {"MOVE", "ALL", "", "0,0", "100,0"});
    undoRedo();
    run("rotate all", {"ROTATE", "ALL", "", "0,0", "30"});
    undoRedo();
    run("scale all", {"SCALE", "ALL", "", "0,0", "1.5"});
    undoRedo();
    run("erase all", {"ERASE", "ALL", ""});
    undoRedo();
    std::filesystem::remove_all(dir);
}
