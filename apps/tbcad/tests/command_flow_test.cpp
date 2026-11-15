// The command line and command flow driven like a person: clicks, keys, the menu.
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <string>

#include "gui.h"
#include "gui_harness.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;
using namespace tbcad::guitest;

namespace {

ImVec2 screenOf(Harness& h, Vec2 world) {
    const Vec2 s = h.core().camera().worldToScreen(world);
    return ImVec2(static_cast<float>(s.x) + ImGui::GetMainViewport()->WorkPos.x,
                  static_cast<float>(s.y) + ImGui::GetMainViewport()->WorkPos.y);
}

void line(Harness& h, const char* text) {
    h.core().submitText(text);
    h.core().pressEscape();
}

}  // namespace

TEST_CASE("command flow: click empty space, then click again, makes a selection window") {
    Harness h(1600, 1000);
    line(h, "LINE 0,0 100,0");
    line(h, "LINE 0,50 100,50");
    line(h, "LINE 0,300 100,300");
    line(h, "ZOOM W -50,-50 150,350");
    h.frame(3);
    const std::size_t before = h.core().document().modelSpaceCount();
    h.command("ERASE");
    h.frame(2);
    const ImVec2 a = screenOf(h, {-20, -20});
    const ImVec2 b = screenOf(h, {120, 80});
    h.click(a.x, a.y);  // first corner: press and release on empty space
    h.frame(2);
    CHECK(h.core().windowCornerPending());
    h.moveMouse((a.x + b.x) / 2, (a.y + b.y) / 2);
    h.frame(2);
    h.moveMouse(b.x, b.y);
    h.frame(2);
    h.click(b.x, b.y);  // second corner
    h.frame(2);
    CHECK_FALSE(h.core().windowCornerPending());
    h.key(ImGuiKey_Enter);
    h.frame(3);
    CHECK(h.core().document().modelSpaceCount() == before - 2);  // the line at y=300 stays
}

TEST_CASE("command flow: a press-drag-release window still works by default") {
    Harness h(1600, 1000);
    line(h, "LINE 0,0 100,0");
    line(h, "LINE 0,300 100,300");
    line(h, "ZOOM W -50,-50 150,350");
    h.frame(3);
    const std::size_t before = h.core().document().modelSpaceCount();
    h.command("ERASE");
    h.frame(2);
    const ImVec2 a = screenOf(h, {-20, -20});
    const ImVec2 b = screenOf(h, {120, 80});
    h.drag(a.x, a.y, b.x, b.y);
    h.frame(2);
    CHECK_FALSE(h.core().windowCornerPending());
    h.key(ImGuiKey_Enter);
    h.frame(3);
    CHECK(h.core().document().modelSpaceCount() == before - 1);
}

TEST_CASE("command flow: Esc closes the right-click menu, and typing goes to the command line") {
    Harness h(1920, 1080);
    line(h, "LINE 100,100 400,100");
    h.frame(3);
    const ImVec2 mid = screenOf(h, {600, 300});
    const float x = mid.x, y = mid.y;
    h.click(x, y, 1);
    h.frame(2);
    REQUIRE(h.has("Select All", "Popup"));
    h.key(ImGuiKey_Escape);
    h.frame(2);
    CHECK_FALSE(h.has("Select All", "Popup"));
    h.click(x, y, 1);
    h.frame(2);
    REQUIRE(h.has("Select All", "Popup"));
    h.type("line");
    h.frame(2);
    CHECK_FALSE(h.has("Select All", "Popup"));
    h.key(ImGuiKey_Enter);
    h.frame(3);
    const auto* p = h.core().runner().prompt();
    CHECK((p && p->command == "LINE"));
}

TEST_CASE("command flow: a mistyped command name (lnie) starts the command it is close to") {
    Harness h(1600, 1000);
    h.frame(3);
    h.command("lnie");
    h.frame(3);
    const auto* p = h.core().runner().prompt();
    CHECK((p && p->command == "LINE"));
}

TEST_CASE("command flow: the command area shows three lines of history above the option chips") {
    Harness h(1600, 1000);
    line(h, "LINE 0,0 100,0");
    h.frame(3);
    h.command("LINE");
    h.frame(2);
    h.command("0,0");
    h.frame(2);
    h.command("100,0");  // now Undo and Close chips are shown
    h.frame(3);
    ImGuiWindow* history = nullptr;
    for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
        if (std::string(w->Name).find("##history") != std::string::npos)
            history = w;
    REQUIRE(history);
    CHECK(history->Size.y >= 3 * ImGui::GetTextLineHeightWithSpacing());
}

TEST_CASE("command flow: TRIM cuts on the first click, with no edge selection and Enter first") {
    Harness h(1600, 1000);
    line(h, "LINE 0,0 100,0");
    line(h, "LINE 50,-50 50,50");
    line(h, "ZOOM W -20,-60 120,60");
    h.frame(3);
    h.command("TRIM");
    h.frame(3);
    const ImVec2 at = screenOf(h, {80, 0});
    h.moveMouse(at.x, at.y);
    h.frame(2);
    h.click(at.x, at.y);
    h.frame(3);
    double maxX = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* l = std::get_if<LineData>(&e.data); l && l->a.y == 0 && l->b.y == 0)
            maxX = std::max(l->a.x, l->b.x);
    });
    CHECK(maxX == doctest::Approx(50));
}
