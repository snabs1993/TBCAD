// Every Toolbox tool, driven the way a person would: started by a click on its button and by
// typing its name, with nothing selected and with a selection made first, and each prompt answered
// by typing a value + Enter, by typing + Space, and by clicking (an option button, else points in
// the drawing). After each run the command must cancel with Esc, the command line must be empty,
// the program must still draw a line, and Dear ImGui must have seen no errors or duplicate ids
// (the harness fails the test for those). Starting by click and by name must run the same command.
#include <doctest/doctest.h>
#include <imgui.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "gui.h"
#include "gui_harness.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/toolbox.h"

using namespace tbcad;
using namespace tbcad::guitest;

namespace {

enum class Start { Click, Typed };
enum class Answer { TypeEnter, TypeSpace, Click };

std::string startName(Start s) {
    return s == Start::Click ? "button" : "typed name";
}
std::string answerName(Answer a) {
    return a == Answer::TypeEnter   ? "type + Enter"
           : a == Answer::TypeSpace ? "type + Space"
                                    : "click";
}

std::string running(Harness& h) {
    const auto* p = h.core().runner().prompt();
    return p ? p->command : std::string();
}

// The first option button of the running prompt that answers it (not a selection mode or the bar's
// own Undo / Hide), or empty.
std::string firstOption(Harness& h) {
    static const std::vector<std::string> skip = {"Undo",  "Hide", "Window", "Crossing",
                                                  "Fence", "All",  "Last",   "Previous"};
    for (const std::string& l : h.labels("optionbar")) {
        if (l.empty() || l.rfind("##", 0) == 0)
            continue;
        if (std::find(skip.begin(), skip.end(), l) == skip.end())
            return l;
    }
    return {};
}

// Runs one tool once; returns the command it started (empty when it opened a panel or did
// something at once).
std::string runTool(const std::string& tab, int nth, const ui::ToolDef& tool, bool selectFirst,
                    Start start, Answer answer) {
    Harness h(1920, 1080);
    h.core().setSetting("ui.toolbox.tab", tab);
    for (const char* line : {"LINE 0,0 100,0 100,100 0,100 c", "CIRCLE 50,50 30"}) {
        h.core().submitText(line);
        h.core().pressEscape();
    }
    if (selectFirst)
        h.core().selectAll();
    h.frame(3);

    if (start == Start::Click) {
        // A row wider than the window scrolls with the wheel; bring the button out from under the
        // scroll arrow at the right edge first.
        for (int i = 0; i < 40; ++i) {
            const imgui_items::Item* b = h.find("##tool", "##toolbox", nth);
            REQUIRE(b);
            if (b->max.x < 1920.0f - 48.0f)
                break;
            h.scroll(960.0f, (b->min.y + b->max.y) * 0.5f, -1.0f);
        }
        REQUIRE(h.click("##tool", "##toolbox", nth));
    } else
        h.command(tool.command);
    h.frame(3);
    const std::string name = running(h);

    const app::ViewportRect vp = h.gui().lastViewportRect();
    for (int step = 0; step < 6 && h.core().runner().active(); ++step) {
        switch (answer) {
            case Answer::TypeEnter:
                h.command(step % 2 ? "80,20" : "50,50");
                break;
            case Answer::TypeSpace:
                h.type(step % 2 ? "70,30" : "30,30");
                h.type(" ");
                break;
            case Answer::Click: {
                const std::string opt = step == 0 ? firstOption(h) : std::string();
                if (opt.empty() || !h.click(opt, "optionbar"))
                    h.click(vp.x + vp.w * (0.4f + 0.05f * float(step)),
                            vp.y + vp.h * (0.4f + 0.04f * float(step)));
                break;
            }
        }
        h.frame(2);
    }
    for (int i = 0; i < 4 && h.core().runner().active(); ++i) {
        h.key(ImGuiKey_Escape);
        h.frame(2);
    }
    for (int i = 0; i < 3; ++i)  // a panel or dialog the tool opened
        h.key(ImGuiKey_Escape);
    h.core().pressEscape();
    h.frame(3);

    CHECK_MESSAGE(!h.core().runner().active(), "Esc did not end the command");
    CHECK_MESSAGE(h.gui().commandLineText().empty(),
                  "text left in the command line: \"" << h.gui().commandLineText() << "\"");
    // Still usable: a line can be drawn.
    const auto before = h.core().document().entityCount();
    h.core().submitText("LINE 200,200 300,250");
    h.core().pressEscape();
    h.frame(2);
    CHECK_MESSAGE(h.core().document().entityCount() > before, "a LINE afterwards drew nothing");
    return name;
}

void sweepTab(const std::string& tabId) {
    const ui::ToolTab* tab = nullptr;
    for (const ui::ToolTab& t : ui::toolboxTabs())
        if (t.id == tabId)
            tab = &t;
    REQUIRE(tab);
    int nth = 0;
    for (const ui::ToolGroup& g : tab->groups)
        for (const ui::ToolDef& tool : g.tools) {
            for (const bool selectFirst : {false, true}) {
                std::map<Start, std::string> started;
                for (const Start start : {Start::Click, Start::Typed})
                    for (const Answer answer :
                         {Answer::TypeEnter, Answer::TypeSpace, Answer::Click}) {
                        CAPTURE(tool.label);
                        CAPTURE(tool.command);
                        CAPTURE(selectFirst);
                        CAPTURE(startName(start));
                        CAPTURE(answerName(answer));
                        started[start] = runTool(tabId, nth, tool, selectFirst, start, answer);
                    }
                CAPTURE(tool.label);
                CAPTURE(selectFirst);
                CHECK_MESSAGE(started[Start::Click] == started[Start::Typed],
                              "the button started \"" << started[Start::Click] << "\" but typing "
                                                      << tool.command << " started \""
                                                      << started[Start::Typed] << "\"");
            }
            ++nth;
        }
}

}  // namespace

TEST_CASE("command sweep: the sweep covers every Toolbox tab") {
    std::vector<std::string> ids;
    for (const ui::ToolTab& t : ui::toolboxTabs())
        ids.push_back(t.id);
    CHECK(ids ==
          std::vector<std::string>{"draw", "modify", "annotate", "blocks", "inspect", "output"});
}

TEST_CASE("command sweep: Draw tab") {
    sweepTab("draw");
}
TEST_CASE("command sweep: Modify tab") {
    sweepTab("modify");
}
TEST_CASE("command sweep: Annotate tab") {
    sweepTab("annotate");
}
TEST_CASE("command sweep: Blocks tab") {
    sweepTab("blocks");
}
TEST_CASE("command sweep: Inspect tab") {
    sweepTab("inspect");
}
TEST_CASE("command sweep: Output tab") {
    sweepTab("output");
}
