// ALIGN, SCALE and ROTATE by reference, driven with mouse clicks that land near, not on, the
// corners of other objects: object snap has to put every point exactly on the geometry.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "tbcad/cmd/register_all.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;

namespace {

struct SnapRig {
    std::filesystem::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;
    EntityId moving{};

    SnapRig() {
        dir = std::filesystem::temp_directory_path() / "tbcad_align_snap";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->applyUiKeybindings(uiRegistry);
        core->setViewportSize(1400, 900);
        core->setSetting("drafting.dynInput", "0");
        core->setSetting("drafting.osnap", "1");
        core->setSetting("drafting.osnap.modes", "endpoint");
        core->setSetting("drafting.ortho", "0");
        core->setSetting("drafting.polar", "0");
        core->setSetting("drafting.snap", "0");
        core->setSetting("drafting.dynucs", "0");
        render::Camera2D cam;
        cam.pixelsPerMm = 6;
        cam.viewportW = 1400;
        cam.viewportH = 900;
        cam.center = {60, 25};
        core->setCamera(cam);
        auto tx = core->document().begin("setup");
        moving = tx.add(line({0, 0}, {10, 0})).value();
        // Guides to snap to: they sit well apart so a click 3 px away finds one end only.
        (void)tx.add(line({100, 0}, {100, 50}));
        (void)tx.add(line({30, 0}, {30, 5}));
        (void)tx.add(line({0, 10}, {5, 10}));
        (void)tx.commit();
    }
    ~SnapRig() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    static Entity line(Vec2 a, Vec2 b) {
        Entity e;
        e.data = LineData{a, b};
        return e;
    }
    // A click 3 pixels off the point in both directions.
    void near(Vec2 world) {
        Vec2 s = core->camera().worldToScreen(world);
        s.x += 3;
        s.y += 2;
        core->click(ui::PointerState{s});
    }
    const LineData& mover() const { return *entityAs<LineData>(*core->document().find(moving)); }
};

bool close(Vec2 a, Vec2 b) {
    return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6;
}

}  // namespace

TEST_CASE("ALIGN with two snapped point pairs turns and scales the object onto the guide") {
    SnapRig r;
    r.core->setSelection({r.moving});
    r.core->submitText("ALIGN");
    r.near({0, 0});
    r.near({100, 0});
    r.near({10, 0});
    r.near({100, 50});
    r.core->submitText("Y");
    CHECK(close(r.mover().a, {100, 0}));
    CHECK(close(r.mover().b, {100, 50}));
    CHECK(r.core->undo());
    CHECK(close(r.mover().b, {10, 0}));
}

TEST_CASE("ALIGN with scale off only turns the object") {
    SnapRig r;
    r.core->setSelection({r.moving});
    r.core->submitText("ALIGN");
    r.near({0, 0});
    r.near({100, 0});
    r.near({10, 0});
    r.near({100, 50});
    r.core->submitText("N");
    CHECK(close(r.mover().a, {100, 0}));
    CHECK(close(r.mover().b, {100, 10}));
}

TEST_CASE("SCALE by reference takes the new length from a snapped point") {
    SnapRig r;
    r.core->setSelection({r.moving});
    r.core->submitText("SCALE");
    r.near({0, 0});
    r.core->submitText("Reference");
    r.near({0, 0});
    r.near({10, 0});
    r.near({30, 0});
    CHECK(close(r.mover().a, {0, 0}));
    INFO("b = " << r.mover().b.x << "," << r.mover().b.y << " a = " << r.mover().a.x << ","
                << r.mover().a.y);
    CHECK(close(r.mover().b, {30, 0}));
}

TEST_CASE("ROTATE by reference takes the new angle from a snapped point") {
    SnapRig r;
    r.core->setSelection({r.moving});
    r.core->submitText("ROTATE");
    r.near({0, 0});
    r.core->submitText("Reference");
    r.near({0, 0});
    r.near({10, 0});
    r.near({0, 10});
    CHECK(close(r.mover().a, {0, 0}));
    CHECK(close(r.mover().b, {0, 10}));
}
