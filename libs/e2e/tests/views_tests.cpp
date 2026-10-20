// Views and coordinates through the command line of the app core: named views with a layer
// snapshot, UCS options, ZOOM history and real time, PAN, the navigation bar and model tiled
// viewports (VPORTS).
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "tbcad/cmd/host.h"
#include "tbcad/cmd/register_all.h"
#include "tbcad/drafting/settings.h"
#include "tbcad/drafting/ucs.h"
#include "tbcad/layers/layer_states.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;

namespace {

struct ViewRig {
    std::filesystem::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;

    explicit ViewRig(const char* tag) {
        dir = std::filesystem::temp_directory_path() / (std::string("tbcad_views_") + tag);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->setViewportSize(1000, 800);
        core->setSetting("drafting.dynInput", "0");
        core->setSetting("drafting.osnap", "0");
        render::Camera2D cam;
        cam.pixelsPerMm = 2;
        cam.viewportW = 1000;
        cam.viewportH = 800;
        cam.center = {0, 0};
        core->setCamera(cam);
    }
    ~ViewRig() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void run(const std::string& command, std::vector<std::string> items = {}) {
        core->submitText(command);
        for (const std::string& i : items)
            core->submitText(i);
        for (int k = 0; k < 3 && core->runner().active(); ++k)
            core->submitText("");
        if (core->runner().active())
            core->pressEscape();
        core->updateFrame();
    }
    Vec2 center() const { return core->camera().center; }
    LayerId addLayer(const char* name) {
        Transaction tx = core->document().begin("layer");
        Layer l;
        l.name = name;
        const LayerId id = tx.addLayer(l).value();
        tx.commit();
        return id;
    }
    bool layerOn(LayerId id) const { return core->document().layers().at(id).on; }
    void setLayerOn(LayerId id, bool on) {
        Transaction tx = core->document().begin("layer");
        REQUIRE(tx.modifyLayer(id, [&](Layer& l) { l.on = on; }));
        tx.commit();
    }
    bool logged(const std::string& text) const {
        for (const std::string& l : core->history())
            if (l.find(text) != std::string::npos)
                return true;
        return false;
    }
    bool hasPanelRequest(const std::string& id) {
        const auto r = core->takePanelRequests();
        return std::find(r.begin(), r.end(), id) != r.end();
    }
    bool hasUiRequest(const std::string& what) {
        const auto r = core->takeUiRequests();
        return std::find(r.begin(), r.end(), what) != r.end();
    }
};

}  // namespace

TEST_CASE("named views keep a layer snapshot that comes back on restore and is hidden") {
    ViewRig r("snapshot");
    const LayerId walls = r.addLayer("ViewSnapshotTest");
    r.run("-VIEW", {"Save", "Plan"});
    const auto& custom = r.core->document().header().custom;
    REQUIRE(custom.count("tbcad.view.Plan") == 1);
    // The snapshot is a layer state the Layer States list does not show.
    for (const std::string& n : layers::layerStateNames(r.core->document()))
        CHECK(n.front() != '*');
    r.setLayerOn(walls, false);
    r.core->setCamera([&] {
        render::Camera2D c = r.core->camera();
        c.center = {500, 500};
        return c;
    }());
    r.run("-VIEW", {"Restore", "Plan"});
    CHECK(r.layerOn(walls));
    CHECK(r.center().x == doctest::Approx(0));
    CHECK(r.center().y == doctest::Approx(0));

    r.run("-VIEW", {"Rename", "Plan", "Ground"});
    CHECK(custom.count("tbcad.view.Plan") == 0);
    REQUIRE(custom.count("tbcad.view.Ground") == 1);
    r.setLayerOn(walls, false);
    r.run("-VIEW", {"Restore", "Ground"});
    CHECK(r.layerOn(walls));  // the snapshot went with the new name

    r.run("-VIEW", {"Category", "Ground", "Floors"});
    CHECK(custom.at("tbcad.view.Ground").find("\tCFloors") != std::string::npos);
    r.run("-VIEW", {"List"});
    CHECK(r.logged("[Floors]"));

    r.run("-VIEW", {"sEttings", "No"});
    r.run("-VIEW", {"Save", "Bare"});
    r.setLayerOn(walls, false);
    r.run("-VIEW", {"Restore", "Bare"});
    CHECK_FALSE(r.layerOn(walls));  // no snapshot: layers stay as they are
}

TEST_CASE("a view saved with a window fits that window when restored") {
    ViewRig r("window");
    r.run("-VIEW", {"Window", "Box", "100,100", "300,200"});
    r.run("ZOOM", {"Extents"});
    r.run("-VIEW", {"Restore", "Box"});
    CHECK(r.center().x == doctest::Approx(200));
    CHECK(r.center().y == doctest::Approx(150));
    // 200 x 100 in a 1000 x 800 window: the width decides, 5 pixels per millimetre.
    CHECK(r.core->camera().pixelsPerMm == doctest::Approx(5));
    r.run("VIEW");
    CHECK(r.hasPanelRequest("views"));
}

TEST_CASE("UCS Z and NAmed rename, UCSFOLLOW, UCSICON properties and UCSMAN") {
    ViewRig r("ucs");
    auto cur = [&] { return drafting::currentUcs(&r.core->document(), r.core.get()); };
    r.run("UCS", {"Z", "90"});
    CHECK(cur().angle == doctest::Approx(kPi / 2));
    r.run("UCS", {"NAmed", "Save", "Turned"});
    r.run("UCS", {"NAmed", "Rename", "Turned", "Quarter"});
    r.run("UCS", {"World"});
    r.run("UCS", {"NAmed", "Restore", "Quarter"});
    CHECK(cur().name == "Quarter");
    CHECK(cur().angle == doctest::Approx(kPi / 2));

    r.run("UCS", {"World"});
    r.run("UCSFOLLOW", {"1"});
    r.run("UCS", {"Origin", "250,40"});
    CHECK(r.center().x == doctest::Approx(250));
    CHECK(r.center().y == doctest::Approx(40));

    r.run("UCSICON", {"Properties", "80", "3"});
    CHECK(r.core->setting(drafting::keys::ucsIconSize).value_or("") == "80");
    CHECK(r.core->setting(drafting::keys::ucsIconColor).value_or("") == "3");
    r.run("UCSICON", {"Properties", "", "Default"});
    CHECK(r.core->setting(drafting::keys::ucsIconColor).value_or("") == "Default");

    r.run("UCSMAN");
    CHECK(r.hasPanelRequest("ucsman"));
}

TEST_CASE("ZOOM Previous steps back through wheel zooms, PAN and views") {
    ViewRig r("previous");
    r.run("ZOOM", {"Previous"});
    CHECK(r.logged("No previous view saved."));
    r.core->zoomAtScreen({500, 400}, 2);
    const double ppm = r.core->camera().pixelsPerMm;
    CHECK(ppm != doctest::Approx(2));
    r.run("-PAN", {"0,0", "100,0"});
    CHECK(r.center().x == doctest::Approx(-100));
    r.run("ZOOM", {"Previous"});
    CHECK(r.center().x == doctest::Approx(0));
    CHECK(r.core->camera().pixelsPerMm == doctest::Approx(ppm));
    r.run("ZOOM", {"Previous"});
    CHECK(r.core->camera().pixelsPerMm == doctest::Approx(2));
    CHECK(cmd::viewHistorySize(*r.core) == 0);
}

TEST_CASE("ZOOM Enter and PAN start real time, ZOOM Dynamic places a box, NAVBAR turns off") {
    ViewRig r("realtime");
    r.run("ZOOM", {""});
    CHECK(r.hasUiRequest("realtime:zoom"));
    r.run("PAN");
    CHECK(r.hasUiRequest("realtime:pan"));
    r.run("ZOOM", {"Dynamic", "40,30", ""});
    CHECK(r.center().x == doctest::Approx(40));
    CHECK(r.center().y == doctest::Approx(30));
    r.run("NAVBAR", {"OFF"});
    CHECK(r.core->setting("ui.navbar").value_or("") == "0");
    r.run("NAVBAR", {"ON"});
    CHECK(r.core->setting("ui.navbar").value_or("") == "1");
}

TEST_CASE("VPORTS tiles model space, activates a tile and saves named configurations") {
    ViewRig r("vports");
    CHECK_FALSE(r.core->tiled());
    r.run("-VPORTS", {"2", "Vertical"});
    REQUIRE(r.core->tiles().size() == 2);
    CHECK(r.core->tiled());
    CHECK(r.core->tileLayout() == "2V");
    CHECK(r.core->setting("tmp.vports.layout").value_or("") == "2V");
    const std::size_t other = r.core->activeTile() == 0 ? 1 : 0;
    r.core->activateTile(other, 0, 0, 500, 800);
    CHECK(r.core->activeTile() == other);
    r.run("-PAN", {"0,0", "50,0"});
    r.run("-VPORTS", {"Save", "Pair"});
    CHECK(r.core->document().header().custom.count("tbcad.vports.Pair") == 1);
    r.run("-VPORTS", {"SIngle"});
    CHECK_FALSE(r.core->tiled());
    r.run("-VPORTS", {"Restore", "Pair"});
    CHECK(r.core->tiles().size() == 2);
    r.run("-VPORTS", {"4"});
    CHECK(r.core->tiles().size() == 4);
    r.run("-VPORTS", {"Delete", "Pair"});
    CHECK(r.core->document().header().custom.count("tbcad.vports.Pair") == 0);
    r.run("VPORTS");
    CHECK(r.hasPanelRequest("vports"));
}
