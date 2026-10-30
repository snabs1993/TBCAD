// Bugs found by the overnight real mouse and keyboard test of test.11, replayed the same way.
#include <doctest/doctest.h>
#include <imgui.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("test.11: a tab's preview does not cover the command line while a command runs") {
    Harness h(1920, 1080);
    h.frame(2);
    const auto* tab = h.find("Layout1");
    REQUIRE(tab != nullptr);
    h.moveMouse((tab->min.x + tab->max.x) / 2, (tab->min.y + tab->max.y) / 2);
    h.frame(90);
    REQUIRE(h.windowOpen("##Tooltip"));
    h.command("LINE");
    h.frame(5);
    CHECK_FALSE(h.windowOpen("##Tooltip"));
    h.key(ImGuiKey_Escape);
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

// Limits found while drawing a sheet from a blank drawing by keyboard.
TEST_CASE("blank drawing: typed variables OSMODE, ORTHOMODE, GRIDMODE, TEXTSIZE, LUPREC") {
    Harness h(1920, 1080);
    h.frame(2);
    auto& c = h.core();
    // Settings outlive the harness: put back what this test changes.
    const char* keys[] = {"drafting.osnap.modes", "drafting.osnap", "drafting.ortho",
                          "drafting.polar",       "view.grid",      "text.height"};
    std::vector<std::pair<std::string, std::string>> kept;
    for (const char* k : keys)
        kept.emplace_back(k, c.setting(k).value_or(""));
    h.command("OSMODE");
    h.command("35");  // endpoint + midpoint + intersection
    h.command("ORTHOMODE");
    h.command("1");
    h.command("GRIDMODE");
    h.command("1");
    h.command("TEXTSIZE");
    h.command("250");
    h.command("LUPREC");
    h.command("2");
    h.frame(3);
    CHECK(c.setting("drafting.osnap.modes").value_or("") == "endpoint,intersection,midpoint");
    CHECK(c.setting("drafting.osnap").value_or("") == "1");
    CHECK(c.setting("drafting.ortho").value_or("") == "1");
    CHECK(c.settings().getBool("view.grid", false));
    CHECK(c.setting("text.height").value_or("") == "250");
    CHECK(linearPrecision(c.document().header()) == 2);
    h.command("SETVAR");
    h.command("OSMODE");
    h.command("16384");  // snap off
    h.frame(2);
    CHECK(c.setting("drafting.osnap").value_or("") == "0");
    for (const auto& [k, v] : kept)
        c.setSetting(k, v);
}

TEST_CASE("blank drawing: -LAYOUT makes a sheet with a viewport; PLOT A1 changes the sheet") {
    Harness h(1920, 1080);
    h.frame(2);
    h.command("RECTANG");
    h.command("0,0");
    h.command("40000,24000");
    h.command("-LAYOUT");
    h.command("New");
    h.command("S-101");
    h.command("A3");
    h.command("Landscape");
    h.frame(3);
    auto lay = layout::findLayout(h.core().document(), "S-101");
    REQUIRE(lay);
    REQUIRE(lay->viewports.size() == 1);
    CHECK(lay->viewports[0].scaleN == doctest::Approx(100));  // 40 m on 400 mm: 1:100
    REQUIRE(h.core().setActiveLayout("S-101"));
    h.frame(2);
    h.command("PLOT");
    h.command("Paper");
    h.command("A1");
    h.frame(2);
    // The sheet follows while the plot options are still open, and the summary says so.
    lay = layout::findLayout(h.core().document(), "S-101");
    REQUIRE(lay);
    CHECK(lay->widthMm == doctest::Approx(841));
    CHECK(lay->heightMm == doctest::Approx(594));
    h.command("Orientation");  // the summary prompt goes to the history once answered
    h.command("Landscape");
    h.frame(2);
    const auto& hist = h.core().history();
    CHECK(std::any_of(hist.begin(), hist.end(), [](const std::string& l) {
        return l.find("841 x 594 mm") != std::string::npos;
    }));
    h.key(ImGuiKey_Escape);
}

TEST_CASE("blank drawing: Select objects takes typed corner points") {
    Harness h(1920, 1080);
    h.frame(2);
    h.command("LINE");
    h.command("0,0");
    h.command("100,0");
    h.command("");
    h.command("LINE");
    h.command("500,500");
    h.command("600,500");
    h.command("");
    h.frame(2);
    REQUIRE(h.core().document().modelSpace().size() == 2);
    h.command("ERASE");
    h.command("-10,-10");
    h.command("200,50");  // left to right: a window around the first line only
    h.command("");
    h.frame(3);
    CHECK(h.core().document().modelSpace().size() == 1);
}

TEST_CASE("blank drawing: text goes on the layer the user made current") {
    Harness h(1920, 1080);
    h.frame(2);
    const Document& d = h.core().document();
    const auto textOn = [&](const char* where) {
        h.command("TEXT");
        h.command(where);
        h.command("");  // height
        h.command("");  // rotation
        h.command("A");
        h.frame(3);
        const Entity* last = nullptr;
        for (EntityId id : d.modelSpace())
            last = d.find(id);
        const Layer* l = last ? d.layer(last->common.layer) : nullptr;
        return l ? l->name : std::string();
    };
    const std::string onZero = textOn("0,0");
    CHECK(onZero != "0");  // layer 0 current: text keeps to its own layer
    h.command("-LAYER");
    h.command("Make");
    h.command("S-NOTE");
    h.command("");
    h.frame(2);
    CHECK(textOn("0,100") == "S-NOTE");
}
