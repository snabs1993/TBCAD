// Draws every registered panel for a few frames in a Dear ImGui context with no window, with the
// library's recoverable-error reporting wired to the test: a mismatched table column count, an
// unbalanced Begin/End or a duplicate ID makes the test fail instead of showing a red box to the
// user. Panels are drawn with an empty drawing, with objects selected and with a command running.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "tbcad/cmd/register_all.h"
#include "tbcad/cmd/registry.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;
namespace fs = std::filesystem;

namespace {

std::vector<std::string>& errors() {
    static std::vector<std::string> e;
    return e;
}

void onError(ImGuiContext*, void*, const char* msg) {
    errors().emplace_back(msg);
}

struct Rig {
    fs::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;

    Rig() {
        dir = fs::temp_directory_path() / "tbcad_panel_frames";
        fs::remove_all(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->setViewportSize(1000, 800);
    }
    ~Rig() {
        core.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

struct ImGuiScope {
    ImGuiContext* ctx;
    ImGuiScope() {
        ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200, 900);
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        ctx->ErrorCallback = onError;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        errors().clear();
    }
    ~ImGuiScope() { ImGui::DestroyContext(ctx); }
};

void drawPanels(Rig& rig, int frames) {
    ui::PanelContext pc;
    pc.doc = &rig.core->document();
    pc.host = rig.core.get();
    pc.selection = &rig.core->selection();
    pc.selectionRevision = rig.core->selectionRevision();
    pc.camera = &rig.core->camera();
    pc.runCommand = [](const std::string&) {};
    pc.cancelCommand = [] {};
    pc.commandActive = rig.core->runner().active();
    for (int f = 0; f < frames; ++f) {
        ImGui::GetIO().DeltaTime = 1.0f / 60;
        ImGui::NewFrame();
        for (const ui::PanelInfo& p : rig.uiRegistry.panels()) {
            if (!p.draw)
                continue;
            ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_Always);
            const std::string title = p.title + "##" + p.id;
            if (ImGui::Begin(title.c_str()))
                p.draw(pc);
            ImGui::End();
        }
        ImGui::Render();
    }
}

void expectNoErrors(const char* situation) {
    INFO(situation);
    for (const std::string& e : errors())
        MESSAGE("ImGui: " << e);
    CHECK(errors().empty());
}

}  // namespace

TEST_CASE("panels draw without Dear ImGui errors: empty drawing") {
    ImGuiScope gui;
    Rig rig;
    drawPanels(rig, 3);
    expectNoErrors("empty drawing");
}

TEST_CASE("panels draw without Dear ImGui errors: drawing with a selection") {
    ImGuiScope gui;
    Rig rig;
    for (const char* line :
         {"LINE 0,0 100,0 100,100 0,100 c", "CIRCLE 50,50 30", "LINE 0,0 100,100",
          "RECTANGLE 10,10 40,40", "TEXT 5,5 2.5 0 Hello", "-LAYER New Walls"}) {
        rig.core->submitText(line);
        rig.core->pressEscape();
    }
    rig.core->selectAll();
    drawPanels(rig, 3);
    expectNoErrors("select all");
    rig.core->clearSelection();
    rig.core->submitText("LINE");
    drawPanels(rig, 2);
    expectNoErrors("command running");
}

// With a big drawing and everything selected (Properties shows *VARIES*), no panel may spend more
// than 1 ms of a frame. Unoptimised builds are only reported, since they are several times slower.
TEST_CASE("panel frame time stays under 1 ms with 393k objects selected") {
    ImGuiScope gui;
    Rig rig;
    rig.core->submitText("DEVGEN LINES 393000");
    for (const char* line :
         {"-LAYER New Walls", "CIRCLE 50,50 30", "TEXT 5,5 2.5 0 Hello", "RECTANGLE 10,10 40,40"}) {
        rig.core->submitText(line);
        rig.core->pressEscape();
    }
    rig.core->selectAll();
    REQUIRE(rig.core->selection().size() > 390000);

    ui::PanelContext pc;
    pc.doc = &rig.core->document();
    pc.host = rig.core.get();
    pc.selection = &rig.core->selection();
    pc.selectionRevision = rig.core->selectionRevision();
    pc.camera = &rig.core->camera();
    pc.runCommand = [](const std::string&) {};
    pc.cancelCommand = [] {};
    std::map<std::string, std::vector<double>> ms;
    constexpr int kFrames = 12;
    for (int f = 0; f < kFrames; ++f) {
        ImGui::GetIO().DeltaTime = 1.0f / 60;
        ImGui::NewFrame();
        for (const ui::PanelInfo& p : rig.uiRegistry.panels()) {
            if (!p.draw)
                continue;
            ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_Always);
            const std::string title = p.title + "##" + p.id;
            if (ImGui::Begin(title.c_str())) {
                const auto t0 = std::chrono::steady_clock::now();
                p.draw(pc);
                const auto t1 = std::chrono::steady_clock::now();
                // The renderer uses 16-bit indices: one window may not draw more vertices.
                if (f == 1)
                    CHECK_MESSAGE(ImGui::GetWindowDrawList()->VtxBuffer.Size < 65000,
                                  "panel " << p.id << " draws too many vertices");
                if (f > 0)  // the first frame builds caches
                    ms[p.id].push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            ImGui::End();
        }
        ImGui::Render();
    }
    expectNoErrors("393k objects selected");
    for (auto& [id, v] : ms) {
        std::sort(v.begin(), v.end());
        const double median = v[v.size() / 2];
        MESSAGE("panel " << id << ": median " << median << " ms, worst " << v.back() << " ms");
#ifdef NDEBUG
        CHECK_MESSAGE(median <= 1.0, "panel " << id << " takes " << median << " ms per frame");
        // One slow frame is a shared runner pausing the thread (8 ms seen on Windows CI); a panel
        // that rebuilds its rows every frame costs 100 ms or more, far above this.
        CHECK_MESSAGE(v.back() <= 30.0, "panel " << id << " has a " << v.back() << " ms frame");
#endif
    }
}
