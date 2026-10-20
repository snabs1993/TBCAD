#include <doctest/doctest.h>

#include <filesystem>
#include <random>

#include "tbcad/cmd/cmd.h"
#include "tbcad/draftingcmds/drafting_cmds.h"
#include "tbcad/ui/app_core.h"

using namespace tbcad;
namespace fs = std::filesystem;

namespace {

class TwoLine final : public cmd::Command {
public:
    std::string_view name() const override { return "TWOLINE"; }
    std::string_view description() const override { return "test"; }
    cmd::Task run(cmd::CommandContext& ctx) override {
        auto a = co_await ctx.getPoint("First point");
        if (!a.ok())
            co_return;
        cmd::PointOptions o;
        o.base = a.point;
        auto b = co_await ctx.getPoint("Second point", o);
        if (!b.ok())
            co_return;
        (void)ctx.tx().add(Entity{{}, LineData{a.point, b.point}});
    }
};

// The real thing minus the window: AppCore with the drafting module's UI hooks installed.
struct App {
    fs::path dir;
    cmd::Registry reg;
    ui::UiRegistry uiReg;
    std::unique_ptr<ui::AppCore> core;

    App() {
        dir = fs::temp_directory_path() /
              ("tbcad_drafting_" + std::to_string(std::random_device{}()));
        fs::create_directories(dir);
        cmd::registerBuiltinCommands(reg);
        draftingcmds::registerCommands(reg);
        (void)reg.add(std::make_unique<TwoLine>());
        core = std::make_unique<ui::AppCore>(reg, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1000, 800);
        render::Camera2D cam = core->camera();
        cam.center = {500, 400};
        cam.pixelsPerMm = 1.0;
        core->setCamera(cam);
        draftingcmds::registerUi(uiReg);
        core->registerBuiltinUi(uiReg);
        core->applyUiKeybindings(uiReg);
    }
    ~App() {
        core.reset();
        draftingcmds::forgetUi();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    ui::PointerState at(Vec2 world) {
        return ui::PointerState{core->camera().worldToScreen(world), false, false};
    }
    void line(Vec2 a, Vec2 b) {
        core->submitText("TWOLINE");
        core->click(at(a));
        core->click(at(b));
    }
    std::vector<LineData> lines() {
        std::vector<LineData> out;
        core->document().forEachEntity([&](const Entity& e) {
            if (const auto* l = entityAs<LineData>(e))
                out.push_back(*l);
        });
        return out;
    }
};

bool near(Vec2 a, Vec2 b) {
    return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6;
}

}  // namespace

TEST_CASE("clicks snap to endpoints, then ortho and polar apply through AppCore") {
    App app;
    app.line({100, 100}, {300, 100});
    REQUIRE(app.lines().size() == 1);

    // Second line starts 4 mm away from the first line's end: osnap (on by default) fixes it.
    app.core->submitText("TWOLINE");
    app.core->click(app.at({303, 103}));
    app.core->pointerMoved(app.at({400, 250}));
    app.core->click(app.at({400, 250}));
    REQUIRE(app.lines().size() == 2);
    CHECK(near(app.lines()[1].a, {300, 100}));
    CHECK(near(app.lines()[1].b, {400, 250}));

    // Ortho (F8's command) makes the next segment horizontal.
    app.core->submitText("TWOLINE");
    app.core->click(app.at({50, 500}));
    app.core->submitText("'TOGGLEORTHO");  // transparent, mid-command
    CHECK(app.core->runner().active());
    app.core->click(app.at({220, 520}));
    REQUIRE(app.lines().size() == 3);
    CHECK(near(app.lines()[2].b, {220, 500}));
}

TEST_CASE("apostrophe commands also work when idle (F-key menu items use them)") {
    App app;
    CHECK(app.core->setting("view.grid").value_or("?") == "0");
    app.core->submitText("'TOGGLEGRID");
    CHECK(app.core->setting("view.grid").value_or("?") == "1");
}

TEST_CASE("typed distance goes the way the polar tracking points") {
    App app;
    app.core->submitText("POLAR on");
    app.core->submitText("POLAR increment 30");
    app.core->submitText("TWOLINE");
    app.core->click(app.at({100, 100}));
    // Cursor at about 31 degrees from the base; polar pulls it to 30.
    const double a = 31 * kPi / 180;
    app.core->pointerMoved(app.at({100 + 200 * std::cos(a), 100 + 200 * std::sin(a)}));
    app.core->submitText("100");
    REQUIRE(app.lines().size() == 1);
    CHECK(near(app.lines()[0].b, {100 + 100 * std::cos(kPi / 6), 100 + 100 * std::sin(kPi / 6)}));
}

TEST_CASE("the status toggles keep ortho and polar exclusive") {
    App app;
    const auto& toggles = app.uiReg.statusToggles();
    auto find = [&](const char* id) -> const ui::StatusToggle& {
        for (const auto& t : toggles)
            if (t.id == id)
                return t;
        FAIL("no such toggle");
        return toggles.front();
    };
    find("polar").set(true);
    find("ortho").set(true);
    CHECK(find("ortho").get());
    CHECK_FALSE(find("polar").get());
    find("polar").set(true);
    CHECK(find("polar").get());
    CHECK_FALSE(find("ortho").get());
}

TEST_CASE("snap markers and the grid are drawn by the overlay - the F-keys are bound") {
    App app;
    app.line({100, 100}, {300, 100});
    app.core->submitText("TWOLINE");
    app.core->pointerMoved(app.at({301, 101}));
    render::Overlay plain;
    app.core->buildOverlay(plain, &app.uiReg);
    CHECK_FALSE(plain.empty());
    render::Overlay noGrid;
    app.core->buildUnderlay(noGrid, &app.uiReg);
    CHECK(noGrid.empty());
    app.core->submitText("'TOGGLEGRID");
    // The grid is in the underlay (drawn before the objects), not in the overlay.
    render::Overlay withGrid;
    app.core->buildUnderlay(withGrid, &app.uiReg);
    CHECK_FALSE(withGrid.empty());
    app.core->pointerMoved(app.at({301, 101}));  // the transparent command cleared the snap marker
    render::Overlay over;
    app.core->buildOverlay(over, &app.uiReg);
    CHECK(over.build(app.core->camera()).vertices.size() ==
          plain.build(app.core->camera()).vertices.size());
    app.core->pressEscape();

    auto chord = app.core->keybindings().chordOf("drafting.osnap");
    REQUIRE(chord);
    CHECK(chord->key == "F3");
    CHECK(app.core->keybindings().chordOf("drafting.polar")->key == "F10");
    CHECK(app.core->keybindings().chordOf("drafting.snapCycle")->key == "TAB");
}

TEST_CASE("a new document gets the filters too") {
    App app;
    app.core->newDocument();
    app.line({0, 0}, {100, 0});
    app.core->submitText("TWOLINE");
    app.core->click(app.at({3, 3}));  // endpoint of the first line is (0,0)
    app.core->click(app.at({500, 500}));
    auto l = app.lines();
    REQUIRE(l.size() == 2);
    CHECK(near(l[1].a, {0, 0}));
}
