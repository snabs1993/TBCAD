// Draws every registered panel for a few frames in a Dear ImGui context with no window, with the
// library's recoverable-error reporting wired to the test: a mismatched table column count, an
// unbalanced Begin/End or a duplicate ID makes the test fail instead of showing a red box to the
// user. Panels are drawn with an empty drawing, with objects selected and with a command running.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
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
