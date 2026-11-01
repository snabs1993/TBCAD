// Bugs from the test.16 redraw of FP-10 (mouse and keyboard only), replayed the same way.
#include <doctest/doctest.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>

#include "gui_harness.h"
#include "tbcad/layout/layout.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;
using namespace tbcad::guitest;

namespace {

ImVec2 screenOf(Harness& h, Vec2 world) {
    const Vec2 s = h.core().camera().worldToScreen(world);
    return ImVec2(static_cast<float>(s.x) + ImGui::GetMainViewport()->WorkPos.x,
                  static_cast<float>(s.y) + ImGui::GetMainViewport()->WorkPos.y);
}

bool printed(Harness& h, const std::string& part, std::size_t from) {
    const auto& hist = h.core().history();
    for (std::size_t i = from; i < hist.size(); ++i)
        if (hist[i].find(part) != std::string::npos)
            return true;
    return false;
}

// A viewport 20,20 - 200,150 on Layout1, made while layer "Viewports" is current.
int viewportOnItsLayer(Harness& h) {
    h.frame(2);
    REQUIRE(h.core().setActiveLayout("Layout1"));
    h.frame(2);
    h.command("-LAYER");
    h.command("Make");
    h.command("Viewports");
    h.command("");
    h.command("MVIEW");
    h.command("20,20");
    h.command("200,150");
    h.command("1:50");
    h.command("0,0");
    h.frame(3);
    const auto l = layout::findLayout(h.core().document(), "Layout1");
    REQUIRE(l);
    REQUIRE(!l->viewports.empty());
    return l->viewports.back().id;
}

}  // namespace

TEST_CASE("test.16: a viewport border goes on the current layer and hides with it") {
    Harness h(1920, 1080);
    const int id = viewportOnItsLayer(h);
    const Document& doc = h.core().document();
    const layout::Viewport* vp = layout::findLayout(doc, "Layout1")->findViewport(id);
    REQUIRE(vp);
    REQUIRE(vp->borderLayer.value != 0);
    CHECK(doc.layer(vp->borderLayer)->name == "Viewports");

    auto borders = [&] {
        layout::LayoutPlotOptions screen;
        screen.screen = true;  // the template's Viewports layer does not plot: look at the screen
        auto sheet = layout::buildLayoutSheet(h.core().document(), "Layout1", {}, screen);
        REQUIRE(sheet);
        int n = 0;
        for (const auto& p : sheet.value().prims)
            if (const auto* path = std::get_if<exportlib::PlotPath>(&p))
                if (path->closed && path->pts.size() >= 4)
                    ++n;
        return n;
    };
    const int shown = borders();
    CHECK(shown >= 1);
    h.command("-LAYER");
    h.command("Off");
    h.command("Viewports");
    h.command("");
    h.frame(2);
    CHECK(borders() == shown - 1);
    h.command("-LAYER");
    h.command("ON");
    h.command("Viewports");
    h.command("Set");  // the current layer cannot be frozen
    h.command("0");
    h.command("Freeze");
    h.command("Viewports");
    h.command("");
    h.frame(2);
    CHECK(borders() == shown - 1);
}

TEST_CASE("test.16: a viewport picked by its border shows in Properties and Esc lets it go") {
    Harness h(1920, 1080);
    const int id = viewportOnItsLayer(h);
    h.core().openPanel("properties");
    h.frame(3);
    const ImVec2 edge = screenOf(h, {20, 80});
    h.click(edge.x, edge.y);
    h.frame(3);
    CHECK(h.core().setting("tmp.layout.viewport").value_or("") ==
          "Layout1:" + std::to_string(id));
    h.captureText(true);
    h.frame(3);
    CHECK(h.shownText().find("Viewport " + std::to_string(id)) != std::string::npos);
    CHECK(h.shownText().find("Delete key or VPDELETE removes it.") != std::string::npos);
    CHECK(h.shownText().find("Nothing selected") == std::string::npos);
    h.captureText(false);
    h.key(ImGuiKey_Escape);
    h.frame(3);
    CHECK(h.core().setting("tmp.layout.viewport").value_or("").empty());
}

TEST_CASE("test.16: ERASE says how to remove a viewport when its border is clicked") {
    Harness h(1920, 1080);
    viewportOnItsLayer(h);
    h.command("ERASE");
    const std::size_t from = h.core().history().size();
    const ImVec2 edge = screenOf(h, {20, 80});
    h.click(edge.x, edge.y);
    h.frame(3);
    CHECK(printed(h, "VPDELETE", from));
    h.core().pressEscape();
    h.frame(2);
}

TEST_CASE("test.16: MVIEW warns when the viewport goes past the sheet") {
    Harness h(1920, 1080);
    h.frame(2);
    REQUIRE(h.core().setActiveLayout("Layout1"));
    h.frame(2);
    const std::size_t from = h.core().history().size();
    h.command("MVIEW");
    h.command("20,20");
    h.command("800,500");
    h.command("1:50");
    h.command("0,0");
    h.frame(3);
    CHECK(printed(h, "outside the sheet", from));
}

TEST_CASE("test.16: DIMTSZ and DIMBLK are typed on the command line") {
    Harness h(1920, 1080);
    h.frame(2);
    const std::size_t from = h.core().history().size();
    h.command("DIMTSZ");
    h.command("1.5");
    h.frame(2);
    CHECK_FALSE(printed(h, "Unknown command", from));
    const DimStyle* s = h.core().document().dimStyle(h.core().document().header().currentDimStyle);
    REQUIRE(s);
    CHECK(s->arrowType == ArrowType::ArchTick);
    CHECK(s->arrowSize == doctest::Approx(1.5));
    h.command("DIMBLK");
    h.command(".");
    h.frame(2);
    s = h.core().document().dimStyle(h.core().document().header().currentDimStyle);
    CHECK(s->arrowType == ArrowType::ClosedFilled);
    h.command("DIMBLK");
    h.command("_OBLIQUE");
    h.frame(2);
    s = h.core().document().dimStyle(h.core().document().header().currentDimStyle);
    CHECK(s->arrowType == ArrowType::ArchTick);
    CHECK_FALSE(printed(h, "Unknown command", from));
}

TEST_CASE("test.16: several pasted lines are entered one by one on the command line") {
    Harness h(1920, 1080);
    h.frame(2);
    h.command("LINE");
    if (const imgui_items::Item* in = h.find("##command")) {
        const imgui_items::Item copy = *in;
        h.clickAt(copy, 0);
    }
    ImGui::SetClipboardText("0,0\n100,0\n100,50\n\n");
    h.key(ImGuiKey_V, true);
    h.frame(3);
    int lines = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (kind(e) == EntityKind::Line)
            ++lines;
    });
    CHECK(lines == 2);
    CHECK_FALSE(h.core().runner().active());
}

TEST_CASE("test.16: the paper picked in the Plot panel becomes the layout's sheet") {
    Harness h(1920, 1080);
    h.frame(2);
    REQUIRE(h.core().setActiveLayout("Layout1"));
    h.frame(2);
    h.command("PLOTPANEL");
    h.frame(5);
    h.captureText(true);
    h.frame(2);
    REQUIRE(h.click("Paper", "Plot"));
    h.frame(3);
    REQUIRE(h.click("A1"));
    h.frame(5);
    const auto l = layout::findLayout(h.core().document(), "Layout1");
    REQUIRE(l);
    CHECK(l->paperName == "A1");
    CHECK(std::max(l->widthMm, l->heightMm) == doctest::Approx(841));
}
