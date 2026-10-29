// Bugs found on the owner's PC with a real mouse and keyboard (test.8, S2312 drawing), each
// replayed here the same way: clicks on the real buttons and in the drawing, keys typed.
#include <doctest/doctest.h>
#include <imgui.h>

#include <optional>
#include <string>
#include <vector>

#include "gui.h"
#include "gui_harness.h"
#include "tbcad/hatch/pattern.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/toolbox.h"

using namespace tbcad;
using namespace tbcad::guitest;

namespace {

std::string running(Harness& h) {
    const auto* p = h.core().runner().prompt();
    return p ? p->command : std::string();
}

void line(Harness& h, const char* text) {
    h.core().submitText(text);
    h.core().pressEscape();
}

// Lines of the command window printed since `from` that tell of an answer the prompt refused.
std::string refusals(Harness& h, std::size_t from) {
    std::string out;
    const auto& hist = h.core().history();
    for (std::size_t i = from; i < hist.size(); ++i)
        if (hist[i].find("Invalid") != std::string::npos ||
            hist[i].find("Unknown command") != std::string::npos ||
            hist[i].find("not a valid") != std::string::npos)
            out += hist[i] + "\n";
    return out;
}

// Clicks Toolbox button `nth` of the tab shown, scrolling it out from under the right arrow.
bool clickTool(Harness& h, int nth) {
    for (int i = 0; i < 40; ++i) {
        const imgui_items::Item* b = h.find("##tool", "##toolbox", nth);
        if (!b)
            return false;
        if (b->max.x < 1920.0f - 48.0f)
            break;
        h.scroll(960.0f, (b->min.y + b->max.y) * 0.5f, -1.0f);
    }
    return h.click("##tool", "##toolbox", nth);
}

void endAll(Harness& h) {
    for (int i = 0; i < 4 && h.core().runner().active(); ++i) {
        h.key(ImGuiKey_Escape);
        h.frame(2);
    }
    for (int i = 0; i < 3; ++i)  // a panel or dialog the tool opened
        h.key(ImGuiKey_Escape);
    h.core().pressEscape();
    h.frame(3);
}

int toolIndex(const std::string& tabId, const std::string& command) {
    for (const ui::ToolTab& t : ui::toolboxTabs()) {
        if (t.id != tabId)
            continue;
        int nth = 0;
        for (const ui::ToolGroup& g : t.groups)
            for (const ui::ToolDef& tool : g.tools) {
                if (tool.command == command)
                    return nth;
                ++nth;
            }
    }
    return -1;
}

EntityId firstOfKind(Harness& h, EntityKind k) {
    EntityId id{};
    h.core().document().forEachEntity([&](const Entity& e) {
        if (kind(e) == k && !id)
            id = e.common.id;
    });
    return id;
}

void clickViewportCentre(Harness& h, float dx = 0, float dy = 0) {
    const app::ViewportRect vp = h.gui().lastViewportRect();
    h.click(vp.x + vp.w * (0.5f + dx), vp.y + vp.h * (0.5f + dy));
}

}  // namespace

TEST_CASE("test.8 HATCH: a pattern clicked in the Hatch panel at Pick internal point is used") {
    Harness h(1920, 1080);
    line(h, "LINE 0,0 100,0 100,100 0,100 c");
    line(h, "ZOOM E");
    h.frame(3);
    h.command("HATCH");
    h.frame(4);
    REQUIRE(running(h) == "HATCH");
    REQUIRE(h.windowOpen("Hatch"));
    // A pattern other than the one in use, near the top of the list so it is on screen.
    const std::vector<std::string> names = hatch::PatternRegistry::names();
    std::string chosen;
    int nth = -1;
    for (int i = 0; i < static_cast<int>(names.size()) && i < 6; ++i) {
        const hatch::Pattern* p = hatch::PatternRegistry::find(names[i]);
        if (p && !p->solid && names[i] != "LINE45") {
            chosen = names[i];
            nth = i;
            break;
        }
    }
    REQUIRE(nth >= 0);
    REQUIRE(h.click("##pattern", "Hatch", nth));
    h.frame(2);
    clickViewportCentre(h);
    h.frame(2);
    h.key(ImGuiKey_Enter);
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    const EntityId id = firstOfKind(h, EntityKind::Hatch);
    REQUIRE(id);
    const auto* hd = entityAs<HatchData>(*h.core().document().find(id));
    REQUIRE(hd);
    CHECK(hd->pattern == chosen);
}

TEST_CASE("test.8: a Toolbox button clicked at another command's prompt starts that tool") {
    // The owner's case: SPLINE waiting for a start tangent, then Rectangle.
    Harness h(1920, 1080);
    h.core().setSetting("ui.toolbox.tab", "draw");
    h.frame(3);
    h.command("SPLINE");
    h.command("0,0");
    h.command("50,20");
    h.command("Tangents");
    h.frame(2);
    REQUIRE(running(h) == "SPLINE");
    const std::size_t from = h.core().history().size();
    const int rect = toolIndex("draw", "RECTANGLE");
    REQUIRE(rect >= 0);
    REQUIRE(clickTool(h, rect));
    h.frame(3);
    CHECK(running(h) == "RECTANGLE");
    CHECK_MESSAGE(refusals(h, from).empty(), refusals(h, from));
    CHECK(firstOfKind(h, EntityKind::Spline) == EntityId{});  // the spline was cancelled
    endAll(h);
}

TEST_CASE("test.8: every Toolbox tool started at a waiting prompt runs as it does from idle") {
    for (const ui::ToolTab& tab : ui::toolboxTabs()) {
        CAPTURE(tab.id);
        Harness h(1920, 1080);
        h.core().setSetting("ui.toolbox.tab", tab.id);
        line(h, "LINE 0,0 100,0 100,100 0,100 c");
        h.frame(3);
        int nth = 0;
        for (const ui::ToolGroup& g : tab.groups)
            for (const ui::ToolDef& tool : g.tools) {
                CAPTURE(tool.command);
                REQUIRE(clickTool(h, nth));
                h.frame(3);
                const std::string fromIdle = running(h);
                endAll(h);

                h.command("LINE");
                h.command("10,10");
                h.frame(2);
                REQUIRE(running(h) == "LINE");
                const std::size_t from = h.core().history().size();
                REQUIRE(clickTool(h, nth));
                h.frame(3);
                CHECK(running(h) == fromIdle);
                CHECK_MESSAGE(refusals(h, from).empty(), refusals(h, from));
                endAll(h);
                ++nth;
            }
    }
}

TEST_CASE("test.8 SPLINE: Enter after the fit points clicked finishes the spline") {
    Harness h(1920, 1080);
    h.frame(3);
    h.command("SPLINE");
    h.frame(2);
    clickViewportCentre(h, -0.2f, 0.1f);
    clickViewportCentre(h, -0.05f, -0.1f);
    clickViewportCentre(h, 0.1f, 0.1f);
    h.frame(2);
    h.key(ImGuiKey_Enter);
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    CHECK(firstOfKind(h, EntityKind::Spline) != EntityId{});
}

TEST_CASE("test.8 Properties of a hatch: area without an exponent, each setting listed once") {
    Harness h(1920, 1080);
    line(h, "LINE 0,0 4000,0 4000,3000 0,3000 c");
    line(h, "ZOOM E");
    h.core().submitText("HATCH");
    h.core().submitText("2000,1500");
    h.core().submitText("");
    h.frame(3);
    const EntityId id = firstOfKind(h, EntityKind::Hatch);
    REQUIRE(id);
    h.core().setSelection({id});
    h.frame(2);
    if (h.has("Object Properties", "DockSpace"))
        h.click("Object Properties", "DockSpace");  // the panel's tab
    h.captureText(true);
    h.frame(3);
    const std::string text = h.shownText();
    CAPTURE(text);
    CHECK(text.find("12000000") != std::string::npos);
    CHECK(text.find("e+0") == std::string::npos);
    // Pattern, scale, angle and origin once, in Geometry (they were also under "Hatch").
    CHECK_FALSE(h.has("Scale", "Object Properties"));
    CHECK_FALSE(h.has("Origin", "Object Properties"));
    CHECK_FALSE(h.has("Pattern", "Object Properties"));
    h.captureText(false);
}

TEST_CASE("test.8 TABLE: a click at the title prompt places the table") {
    Harness h(1920, 1080);
    h.frame(3);
    h.command("TABLE");
    for (int i = 0; i < 4; ++i)  // columns, rows, width and height: the defaults
        h.command("");
    h.frame(2);
    REQUIRE(running(h) == "TABLE");
    clickViewportCentre(h);
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    CHECK(firstOfKind(h, EntityKind::Table) != EntityId{});
}

TEST_CASE("test.8 File > Open: keys typed while the dialog is up do not run as commands") {
    Harness h(1920, 1080);
    h.frame(3);
    bool shown = false;
    h.gui().fileDialogForTests = [&]() -> std::optional<std::filesystem::path> {
        // What reached ImGui from the OS while the person typed in the dialog.
        shown = true;
        ImGuiIO& io = ImGui::GetIO();
        io.AddInputCharactersUTF8("LINE");
        io.AddKeyEvent(ImGuiKey_Enter, true);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        return std::nullopt;  // cancelled
    };
    REQUIRE(h.click("File", "MainMenuBar"));
    h.frame(2);
    REQUIRE(h.click("Open...", "##Menu"));
    h.frame(5);
    CHECK(shown);
    CHECK_FALSE(h.core().runner().active());
    CHECK(h.gui().commandLineText().empty());
    h.gui().fileDialogForTests = nullptr;
}
