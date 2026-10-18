// Automated GUI tests: the real Gui is driven with queued mouse and keyboard events, without a
// window or a GPU (see gui_harness.h). They stand in for a person clicking through the program.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "gui_harness.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;
using namespace tbcad::guitest;

TEST_CASE("probe: list the widgets of the first frames") {
    Harness h;
    h.frame(5);
    std::string all;
    for (const auto& it : tbcad::imgui_items::items())
        all += it.window + " | " + it.label + " @" + std::to_string(int(it.min.x)) + "," +
               std::to_string(int(it.min.y)) + "\n";
    FILE* f = std::fopen("/tmp/items.txt", "w");
    std::fputs(all.c_str(), f);
    std::fclose(f);
    Image shot = h.screenshot();
    CHECK(shot.width == 1440);
    matchesGolden(shot, "probe");
}
