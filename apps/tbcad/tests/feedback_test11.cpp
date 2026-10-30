// Bugs found by the overnight real mouse and keyboard test of test.11, replayed the same way.
#include <doctest/doctest.h>
#include <imgui.h>

#include <algorithm>

#include <string>

#include "gui_harness.h"
#include "tbcad/layout/layout.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;
using namespace tbcad::guitest;

TEST_CASE("test.11: Esc closes an open menu, one level at a time") {
    Harness h(1920, 1080);
    h.frame(2);
    REQUIRE(h.click("View"));
    h.frame(3);
    REQUIRE(h.has("Zoom Extents", "##Menu"));
    h.key(ImGuiKey_Escape);
    h.frame(3);
    CHECK_FALSE(h.has("Zoom Extents", "##Menu"));
}

TEST_CASE("test.11: Delete removes the viewport picked on the sheet") {
    Harness h(1920, 1080);
    h.frame(2);
    REQUIRE(h.core().setActiveLayout("Layout1"));
    h.frame(2);
    h.command("MVIEW");
    h.command("20,20");
    h.command("200,150");
    h.command("banana");  // asked again, the corners are kept
    h.command("1:50");
    h.command("0,0");
    h.frame(3);
    auto count = [&] {
        const auto l = layout::findLayout(h.core().document(), "Layout1");
        return l ? l->viewports.size() : std::size_t{0};
    };
    const std::size_t before = count();
    REQUIRE(before >= 1);
    const int id = layout::findLayout(h.core().document(), "Layout1")->viewports.back().id;
    h.core().setSetting("tmp.layout.viewport", "Layout1:" + std::to_string(id));
    h.key(ImGuiKey_Delete);
    h.frame(3);
    CHECK(count() == before - 1);
}

TEST_CASE("test.11: LTSCALE changes the dashes already drawn") {
    Harness h(1920, 1080);
    h.frame(2);
    auto maxPeriod = [&] {
        double m = 0;
        for (const auto& [k, tile] : h.core().scene().tiles())
            for (const auto& [bk, b] : tile.batches)
                m = std::max(m, h.core().scene().pattern(bk.pattern).period());
        return m;
    };
    h.command("-LAYER");
    h.command("Linetype");
    h.command("DASHED");
    h.command("0");
    h.key(ImGuiKey_Enter);
    h.frame(2);
    h.command("LINE");
    h.command("0,0");
    h.command("1000,0");
    h.key(ImGuiKey_Enter);
    h.frame(4);
    const double before = maxPeriod();
    REQUIRE(before > 0);
    h.command("LTSCALE");
    h.command("10");
    h.frame(4);
    CHECK(maxPeriod() == doctest::Approx(before * 10));
}

TEST_CASE("test.11: the Interface scale slider can be dragged") {
    Harness h(1920, 1080);
    h.frame(2);
    h.command("OPTIONS");
    h.frame(4);
    REQUIRE(h.click("Display"));
    h.frame(3);
    const auto* slider = h.find("Interface scale (%)");
    REQUIRE(slider != nullptr);
    const float y = (slider->min.y + slider->max.y) / 2;
    const float x0 = slider->min.x + (slider->max.x - slider->min.x) * 0.2f;
    const float x1 = slider->min.x + (slider->max.x - slider->min.x) * 0.6f;
    h.drag(x0, y, x1, y);
    h.frame(4);
    CHECK(h.core().settings().getNumber("ui.scale", 1.0) > 1.2);
    h.core().settings().setNumber("ui.scale", 1.0);
}
