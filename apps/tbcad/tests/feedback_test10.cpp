// Bugs from the owner's test.10 round (mouse and keyboard on the S2312 drawing), replayed with
// real clicks and keys.
#include <doctest/doctest.h>
#include <imgui.h>

#include <string>

#include "gui.h"
#include "gui_harness.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/toolbox.h"

using namespace tbcad;
using namespace tbcad::guitest;

namespace {

std::string running(Harness& h) {
    const auto* p = h.core().runner().prompt();
    return p ? p->command : std::string();
}

std::string promptText(Harness& h) {
    const auto* p = h.core().runner().prompt();
    return p ? p->text : std::string();
}

void line(Harness& h, const char* text) {
    h.core().submitText(text);
    h.core().pressEscape();
}

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
    for (int i = 0; i < 3; ++i)
        h.key(ImGuiKey_Escape);
    h.core().pressEscape();
    h.frame(3);
}

}  // namespace

TEST_CASE("test.10: every Modify tab button answers, with and without a selection") {
    const ui::ToolTab* modify = nullptr;
    for (const ui::ToolTab& t : ui::toolboxTabs())
        if (t.id == "modify")
            modify = &t;
    REQUIRE(modify);
    for (const bool selected : {false, true}) {
        CAPTURE(selected);
        Harness h(1920, 1080);
        h.core().setSetting("ui.toolbox.tab", "modify");
        line(h, "LINE 0,0 100,0 100,100 0,100 c");
        line(h, "COPYCLIP ALL");
        h.frame(3);
        int nth = 0;
        for (const ui::ToolGroup& g : modify->groups)
            for (const ui::ToolDef& tool : g.tools) {
                CAPTURE(tool.command);
                if (selected) {
                    h.core().submitText("SELECT");
                    h.core().submitText("ALL");
                    h.core().submitText("");
                    h.frame(2);
                }
                const std::size_t from = h.core().history().size();
                const std::uint64_t rev = h.core().document().revision();
                REQUIRE(clickTool(h, nth));
                h.frame(3);
                // Something must show: a prompt, a line in the command window or a change.
                const bool answered = h.core().runner().active() ||
                                      h.core().history().size() > from ||
                                      h.core().document().revision() != rev;
                CHECK_MESSAGE(answered, tool.command << " did nothing");
                endAll(h);
                ++nth;
            }
    }
}

TEST_CASE("test.10: ROTATE and SCALE Reference take a typed number as the angle and length") {
    Harness h(1920, 1080);
    line(h, "LINE 0,0 100,0");
    h.frame(2);
    h.command("ROTATE");
    h.command("ALL");
    h.command("");
    h.command("0,0");
    h.command("R");
    h.command("45");
    h.frame(2);
    CHECK(promptText(h).find("new angle") != std::string::npos);
    endAll(h);
    h.command("SCALE");
    h.command("ALL");
    h.command("");
    h.command("0,0");
    h.command("R");
    h.command("100");
    h.frame(2);
    CHECK(promptText(h).find("new length") != std::string::npos);
    endAll(h);
}

TEST_CASE("test.10: -INSERT typed asks for the block name, not a point") {
    Harness h(1920, 1080);
    line(h, "CIRCLE 0,0 10");
    h.core().submitText("-BLOCK");
    h.core().submitText("B1");
    h.core().submitText("ALL");
    h.core().submitText("");
    h.core().submitText("0,0");
    h.core().submitText("Retain");
    h.frame(2);
    h.command("-INSERT");
    h.frame(4);
    CHECK(running(h) == "-INSERT");
    CHECK(promptText(h).find("block name") != std::string::npos);
    h.command("B1");
    h.frame(2);
    std::string tail;
    for (std::size_t i = h.core().history().size() > 8 ? h.core().history().size() - 8 : 0;
         i < h.core().history().size(); ++i)
        tail += h.core().history()[i] + "\n";
    CHECK_MESSAGE(promptText(h).find("insertion point") != std::string::npos, tail);
    endAll(h);
}

TEST_CASE("test.10: Space after the selection and the base point typed at once, MOVE keeps it") {
    Harness h(1920, 1080);
    line(h, "LINE 0,0 100,0");
    h.frame(2);
    h.command("MOVE");
    h.command("ALL");
    h.frame(2);
    REQUIRE(running(h) == "MOVE");
    // Fast keys: Space, the point and Enter all in one go.
    ImGuiIO& io = ImGui::GetIO();
    io.AddInputCharactersUTF8(" 10,0");
    io.AddKeyEvent(ImGuiKey_Enter, true);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    h.frame(6);
    CHECK(promptText(h).find("second point") != std::string::npos);
    endAll(h);
}

TEST_CASE("test.10: changing the Interface scale in the middle of a frame does not crash") {
    Harness h(1920, 1080);
    h.frame(2);
    h.core().settings().setNumber("ui.scale", 1.5);
    (void)h.overlayShot(64, 64, [&](ImDrawList*) { h.gui().setDpiScale(h.gui().dpiScale()); });
    h.frame(3);
    h.core().settings().setNumber("ui.scale", 1.0);
    h.gui().setDpiScale(h.gui().dpiScale());
    h.frame(3);
    CHECK(ImGui::GetFontSize() > 0);
}

TEST_CASE("test.10: Reset all settings in the Options dialog does not crash") {
    Harness h(1920, 1080);
    h.frame(2);
    h.core().settings().setNumber("ui.scale", 1.25);
    h.command("OPTIONS");
    h.frame(4);
    REQUIRE(h.click("Reset all settings..."));
    h.frame(3);
    REQUIRE(h.click("Reset", "Reset settings?"));
    h.frame(6);
    CHECK(h.core().settings().getNumber("ui.scale", 1.0) == doctest::Approx(1.0));
}

TEST_CASE("test.10: GRID on in a new drawing draws and returns") {
    Harness h(1920, 1080);
    h.frame(2);
    h.command("GRID");
    h.command("ON");
    h.frame(4);
    for (int i = 0; i < 6; ++i)
        h.scroll(960.0f, 540.0f, i % 2 ? 3.0f : -3.0f);
    h.frame(3);
    CHECK(h.core().settings().getBool("view.grid", false));
}

TEST_CASE("test.10: in a small window the Modify buttons past the scroll arrow still answer") {
    const ui::ToolTab* modify = nullptr;
    for (const ui::ToolTab& t : ui::toolboxTabs())
        if (t.id == "modify")
            modify = &t;
    REQUIRE(modify);
    int total = 0;
    for (const ui::ToolGroup& g : modify->groups)
        total += static_cast<int>(g.tools.size());
    Harness h(1280, 720);
    h.core().settings().setNumber("ui.scale", 1.25);  // the owner's PC runs at 125 %
    h.gui().setDpiScale(h.gui().dpiScale());
    h.core().setSetting("ui.toolbox.tab", "modify");
    line(h, "LINE 0,0 100,0 100,100 0,100 c");
    line(h, "COPYCLIP ALL");
    h.frame(3);
    for (int nth = total - 10; nth < total; ++nth) {
        CAPTURE(nth);
        // Scroll the way a person does: clicks on the right arrow until the button is clear.
        for (int i = 0; i < 10; ++i) {
            const imgui_items::Item* b = h.find("##tool", "##toolbox", nth);
            REQUIRE(b);
            if (b->max.x < 1280.0f - 30.0f)
                break;
            h.click(1280.0f - 8.0f, (b->min.y + b->max.y) * 0.5f);
            h.frame(2);
        }
        const std::size_t from = h.core().history().size();
        const std::uint64_t rev = h.core().document().revision();
        REQUIRE(h.click("##tool", "##toolbox", nth));
        h.frame(3);
        const bool answered = h.core().runner().active() || h.core().history().size() > from ||
                              h.core().document().revision() != rev;
        CHECK(answered);
        endAll(h);
    }
}
