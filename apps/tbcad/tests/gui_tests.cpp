// Automated GUI tests: the real Gui is driven with queued mouse and keyboard events, without a
// window or a GPU (see gui_harness.h). They stand in for a person clicking through the program,
// so a panel that compiles but does nothing, or draws wrongly, fails here.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "gui_harness.h"
#include "tbcad/layers/layer_states.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/xref/xref.h"

using namespace tbcad;
using namespace tbcad::guitest;
namespace fs = std::filesystem;

namespace {

const Layer* layer(Harness& h, const char* name) {
    return h.core().document().layerByName(name);
}

// Runs a command line the way the command window does, ends it (-LAYER asks for another option)
// and lets the frames catch up.
void run(Harness& h, const char* line) {
    h.core().submitText(line);
    h.core().pressEscape();
    h.frame(2);
}

// Three layers with one line each.
void threeLayers(Harness& h) {
    run(h, "-LAYER New A-Walls,A-Doors,A-Notes");
    for (const char* l : {"A-Walls", "A-Doors", "A-Notes"}) {
        run(h, (std::string("-LAYER Make ") + l).c_str());
        h.core().submitText("LINE");
        h.core().submitText("0,0");
        h.core().submitText("10,0");
        h.core().pressEscape();
    }
    h.frame(3);
}

}  // namespace

TEST_CASE("layers panel: filters, invert and the filter box") {
    Harness h(1920, 1080);
    threeLayers(h);
    run(h, "-LAYER Off A-Doors");
    CHECK(h.has("A-Walls", "Layers"));
    CHECK(h.has("A-Doors", "Layers"));

    // Ready-made filter from the drop-down.
    REQUIRE(h.click("##filters", "Layers"));
    REQUIRE(h.click("Layers that are off", "Combo"));
    h.frame(2);
    CHECK(h.has("A-Doors", "Layers"));
    CHECK_FALSE(h.has("A-Walls", "Layers"));
    CHECK_FALSE(h.has("A-Notes", "Layers"));

    // Invert shows the others.
    REQUIRE(h.click("Invert", "Layers"));
    h.frame(2);
    CHECK_FALSE(h.has("A-Doors", "Layers"));
    CHECK(h.has("Defpoints", "Layers"));
    REQUIRE(h.click("Invert", "Layers"));

    // Back to everything, then type a name into the filter box.
    REQUIRE(h.click("##filters", "Layers"));
    REQUIRE(h.click("All layers", "Combo"));
    h.frame(2);
    CHECK(h.has("Defpoints", "Layers"));
    REQUIRE(h.enterText("##filter", "a-wal", "Layers"));
    h.frame(2);
    CHECK(h.has("A-Walls", "Layers"));
    CHECK_FALSE(h.has("A-Notes", "Layers"));
    CHECK_FALSE(h.has("A-Doors", "Layers"));
    CHECK(matchesGolden(h.windowShot("Layers"), "layers_filter_text"));
}

TEST_CASE("layers panel: switches on the rows change the drawing") {
    Harness h(1920, 1080);
    threeLayers(h);
    REQUIRE(layer(h, "A-Doors"));
    CHECK(layer(h, "A-Doors")->on);
    // The template's own layers fill the panel, so show only ours; rows are sorted by name.
    REQUIRE(h.enterText("##filter", "a-", "Layers"));
    h.frame(2);
    REQUIRE(h.click("##on", "Layers", 0));
    h.frame(2);
    CHECK_FALSE(layer(h, "A-Doors")->on);
    REQUIRE(h.click("##lck", "Layers", 2));
    h.frame(2);
    CHECK(layer(h, "A-Walls")->locked);
    CHECK_FALSE(layer(h, "A-Notes")->locked);
    // Undo through the command window puts them back.
    h.command("U");
    h.frame(2);
    CHECK_FALSE(layer(h, "A-Walls")->locked);
}

TEST_CASE("layers panel: groups and merge") {
    Harness h(1920, 1080);
    threeLayers(h);

    REQUIRE(h.enterText("##filter", "a-", "Layers"));
    h.frame(2);
    // Select Walls and Doors with Ctrl-click, make a group out of them.
    REQUIRE(h.click("A-Doors", "Layers"));
    h.holdCtrl(true);
    REQUIRE(h.click("A-Walls", "Layers"));
    h.holdCtrl(false);
    REQUIRE(h.click("New group...", "Layers"));
    h.frame(2);
    REQUIRE(h.enterText("##name", "Frame", "Popup"));
    h.frame(2);
    const auto groups = layers::layerGroupNames(h.core().document());
    REQUIRE(groups.size() == 1);
    CHECK(groups[0] == "frame");

    // Pick the group in the Filters drop-down.
    REQUIRE(h.click("##filters", "Layers"));
    REQUIRE(h.click("frame", "Combo"));
    h.frame(2);
    CHECK(h.has("A-Walls", "Layers"));
    CHECK(h.has("A-Doors", "Layers"));
    CHECK_FALSE(h.has("A-Notes", "Layers"));

    // Merge Doors into Walls from the row's context menu.
    REQUIRE(h.click("A-Doors", "Layers"));
    REQUIRE(h.rightClick("A-Doors", "Layers"));
    h.frame(2);
    REQUIRE(h.click("Merge Into", "Popup"));
    h.frame(2);
    REQUIRE(h.click("A-Walls", "Menu"));
    h.frame(3);
    CHECK_FALSE(layer(h, "A-Doors"));
    REQUIRE(layer(h, "A-Walls"));
    std::size_t onWalls = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (e.common.layer == layer(h, "A-Walls")->id)
            ++onWalls;
    });
    CHECK(onWalls == 2);
}

TEST_CASE("quick properties: change the layer and a size of the selection") {
    Harness h(1920, 1080);
    threeLayers(h);
    h.core().setSetting("ui.quickProps", "1");
    h.core().selectAll();
    h.frame(3);
    REQUIRE(h.windowOpen("##quickprops"));
    CHECK(h.has("General", "##quickprops"));

    // Move all three objects to one layer from the Layer drop-down.
    REQUIRE(h.click("##layer", "##quickprops"));
    REQUIRE(h.click("A-Notes", "Combo"));
    h.frame(3);
    std::size_t onNotes = 0, all = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        ++all;
        if (e.common.layer == layer(h, "A-Notes")->id)
            ++onNotes;
    });
    CHECK(all == 3);
    CHECK(onNotes == 3);
    // The same through undo.
    h.command("U");
    h.frame(3);
    onNotes = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (e.common.layer == layer(h, "A-Notes")->id)
            ++onNotes;
    });
    CHECK(onNotes == 1);
}

TEST_CASE("quick properties: a circle's radius field") {
    Harness h(1920, 1080);
    run(h, "-LAYER New A-Round");
    h.core().submitText("CIRCLE");
    h.core().submitText("0,0");
    h.core().submitText("10");
    h.core().selectAll();
    h.core().setSetting("ui.quickProps", "1");
    h.frame(3);
    REQUIRE(h.windowOpen("##quickprops"));
    // Editable fields in order: centre X, centre Y, radius, diameter ...
    REQUIRE(h.enterText("##v", "25", "##quickprops", 2));
    h.frame(3);
    double radius = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* c = std::get_if<CircleData>(&e.data))
            radius = c->radius;
    });
    CHECK(radius == doctest::Approx(25.0));
    // Typing the diameter changes the radius too.
    REQUIRE(h.enterText("##v", "30", "##quickprops", 3));
    h.frame(3);
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* c = std::get_if<CircleData>(&e.data))
            radius = c->radius;
    });
    CHECK(radius == doctest::Approx(15.0));
    CHECK(matchesGolden(h.windowShot("##quickprops"), "quickprops_circle"));
}

TEST_CASE("drafting settings: snap, grid, polar, object snap and selection tabs") {
    Harness h(1920, 1080);
    h.core().submitText("DSETTINGS");
    h.frame(3);
    REQUIRE(h.windowOpen("Settings"));
    auto on = [&](const char* key) { return h.core().setting(key).value_or("0") == "1"; };

    // Snap and Grid tab (the one DSETTINGS opens).
    CHECK_FALSE(on("view.grid"));
    REQUIRE(h.click("Grid on (F7)", "Settings"));
    h.frame(2);
    CHECK(on("view.grid"));
    REQUIRE(h.click("Snap on (F9)", "Settings"));
    h.frame(2);
    CHECK(on("drafting.snap"));
    REQUIRE(h.click("Grid on (F7)", "Settings"));
    h.frame(2);
    CHECK_FALSE(on("view.grid"));

    // Polar Tracking: polar and ortho exclude each other.
    REQUIRE(h.click("Polar Tracking", "Settings"));
    h.frame(2);
    REQUIRE(h.click("Ortho on (F8)", "Settings"));
    h.frame(2);
    CHECK(on("drafting.ortho"));
    REQUIRE(h.click("Polar tracking on (F10)", "Settings"));
    h.frame(2);
    CHECK(on("drafting.polar"));
    CHECK_FALSE(on("drafting.ortho"));
    REQUIRE(h.enterText("Increment angles (deg)", "30,45", "Settings"));
    h.frame(2);
    CHECK(h.core().setting("drafting.polar.increment").value_or("") == "30,45");

    // Object Snap: Quadrant is on by default; switch it off.
    REQUIRE(h.click("Object Snap", "Settings"));
    h.frame(2);
    CHECK(h.core().setting("drafting.osnap.modes").value_or("quadrant").find("quadrant") !=
          std::string::npos);
    REQUIRE(h.click("Quadrant", "Settings"));
    h.frame(2);
    CHECK(h.core().setting("drafting.osnap.modes").value_or("").find("quadrant") ==
          std::string::npos);
    CHECK(matchesGolden(h.windowShot("Settings"), "settings_object_snap"));

    // Selection: PICKADD and PICKFIRST.
    REQUIRE(h.click("Selection", "Settings"));
    h.frame(2);
    REQUIRE(h.click("Picks add to the selection (PICKADD)", "Settings"));
    h.frame(2);
    CHECK(h.core().setting("selection.pickadd").value_or("") == "0");
    REQUIRE(h.click("Select objects before a command (PICKFIRST)", "Settings"));
    h.frame(2);
    CHECK(h.core().setting("selection.pickfirst").value_or("") == "0");

    // Close button and Esc both close the dialog.
    REQUIRE(h.click("Close", "Settings"));
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Settings"));
}

namespace {

fs::path scratchDir(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("tbcad_guitest_") + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("external references panel: attach, unload, reload, detach") {
    Harness h(1920, 1080);
    // A drawing to refer to: one line, saved, then a fresh drawing to attach it to.
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,0");
    h.core().pressEscape();
    const fs::path dir = scratchDir("xref");
    const fs::path source = dir / "source.tbcad";
    REQUIRE(h.core().saveAs(source).ok());
    h.core().newDocument();
    h.frame(3);
    h.core().submitText("XATTACH");
    h.core().submitText(source.string());
    h.core().submitText("Attach");
    h.core().submitText("Absolute");
    h.core().submitText("source");
    h.core().submitText("0,0");
    h.core().pressEscape();
    h.frame(3);
    const auto refs = [&] { return xref::xrefs(h.core().document()); };
    REQUIRE(refs().size() == 1);
    const BlockId id = refs()[0];
    const auto state = [&] { return xref::state(h.core().document(), id, {}); };
    CHECK(state() == xref::State::UpToDate);

    // The EXTERNALREFERENCES command opens the panel; its tab shows the one reference.
    h.core().submitText("XREF");
    h.frame(3);
    REQUIRE(h.windowOpen("External References"));
    CHECK(h.has("Attach...", "External References"));
    REQUIRE(h.click("source", "External References"));
    h.frame(2);
    CHECK(h.has("Detach", "External References"));

    REQUIRE(h.click("Unload", "External References"));
    h.frame(3);
    CHECK(state() == xref::State::Unloaded);
    REQUIRE(h.click("Reload", "External References"));
    h.frame(3);
    CHECK(state() == xref::State::UpToDate);

    // Reload all after the source file changed on disk.
    REQUIRE(h.click("Reload all", "External References"));
    h.frame(3);
    CHECK(state() == xref::State::UpToDate);

    REQUIRE(h.click("Detach", "External References"));
    h.frame(3);
    CHECK(refs().empty());
    CHECK(h.has("Attach...", "External References"));
}

TEST_CASE("plot panel: options change the exported PDF") {
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("200,100");
    h.core().pressEscape();
    h.core().submitText("PLOTPANEL");
    h.frame(3);
    REQUIRE(h.click("Plot", "DockSpace"));  // the panel's tab in the right-hand dock
    h.frame(3);
    REQUIRE(h.windowOpen("Plot"));

    const fs::path dir = scratchDir("plot");
    const fs::path landscape = dir / "landscape.pdf";
    const fs::path portrait = dir / "portrait.pdf";
    // The paper's width and height, from the page's /MediaBox.
    auto mediaBox = [&](const fs::path& file) {
        const std::string pdf = slurp(file);
        const auto at = pdf.find("/MediaBox");
        REQUIRE(at != std::string::npos);
        double x0, y0, w, hgt;
        REQUIRE(std::sscanf(pdf.c_str() + at, "/MediaBox [%lf %lf %lf %lf", &x0, &y0, &w, &hgt) ==
                4);
        return std::pair<double, double>{w, hgt};
    };

    REQUIRE(h.click("Landscape", "Plot"));
    REQUIRE(h.enterText("File", landscape.string(), "Plot"));
    REQUIRE(h.click("Export PDF", "Plot"));
    h.frame(5);
    REQUIRE(fs::exists(landscape));
    CHECK(slurp(landscape).rfind("%PDF", 0) == 0);
    const auto land = mediaBox(landscape);
    CHECK(land.first > land.second);

    REQUIRE(h.click("Portrait", "Plot"));
    REQUIRE(h.enterText("File", portrait.string(), "Plot"));
    REQUIRE(h.click("Export PDF", "Plot"));
    h.frame(5);
    REQUIRE(fs::exists(portrait));
    const auto port = mediaBox(portrait);
    CHECK(port.first < port.second);
    CHECK(port.first == doctest::Approx(land.second).epsilon(0.01));
}

namespace {

// Pixels of the X arrow's red inside the viewport (x < 1420, between the toolbar and the tabs).
struct RedSpot {
    int count = 0;
    double cx = 0, cy = 0;
};
RedSpot redSpot(const Image& img) {
    RedSpot r;
    for (int y = 95; y < 860 && y < img.height; ++y)
        for (int x = 0; x < 1420 && x < img.width; ++x) {
            const std::uint8_t* p = &img.rgba[(static_cast<std::size_t>(y) * img.width + x) * 4];
            if (p[0] > 200 && p[1] < 110 && p[2] < 110) {
                ++r.count;
                r.cx += x;
                r.cy += y;
            }
        }
    if (r.count) {
        r.cx /= r.count;
        r.cy /= r.count;
    }
    return r;
}

}  // namespace

TEST_CASE("UCS icon: on, off, origin, noorigin, rotated and the world marker") {
    Harness h(1920, 1080);
    h.core().setSetting("display.originIcon", "0");  // the plain axes landmark, not under test
    h.core().submitText("LINE");  // the start page hides the icon while the drawing is untouched
    h.core().submitText("500,500");
    h.core().submitText("900,500");
    h.core().pressEscape();
    h.frame(3);
    Vec2 o = h.core().camera().worldToScreen({0, 0});
    o.y += 92;  // the viewport starts below the menu, toolbar and tabs

    // On by default, at the world origin.
    const RedSpot on = redSpot(h.screenshot());
    CHECK(on.count > 20);
    CHECK(on.cx == doctest::Approx(o.x + 18).epsilon(0.5));  // the arrow lies to the right
    const Image atOrigin =
        crop(h.screenshot(), static_cast<int>(o.x) - 30, static_cast<int>(o.y) - 70, 130, 110);
    CHECK(matchesGolden(atOrigin, "ucsicon_world"));

    // Noorigin: the icon moves to the lower-left corner of the viewport.
    run(h, "UCSICON Noorigin");
    const RedSpot corner = redSpot(h.screenshot());
    CHECK(corner.count > 20);
    CHECK(corner.cx < 120);
    CHECK(corner.cy > 700);
    run(h, "UCSICON ORigin");
    CHECK(redSpot(h.screenshot()).cx == doctest::Approx(on.cx).epsilon(0.01));

    // Rotated UCS: the X arrow turns, the world marker goes away.
    h.core().submitText("UCS");
    h.core().submitText("Rotate");
    h.core().submitText("90");
    h.frame(3);
    const RedSpot turned = redSpot(h.screenshot());
    CHECK(turned.count > 20);
    CHECK(turned.cy < on.cy - 10);  // pointing up now (screen y grows downwards)
    const Image rotated =
        crop(h.screenshot(), static_cast<int>(o.x) - 30, static_cast<int>(o.y) - 70, 130, 110);
    CHECK(matchesGolden(rotated, "ucsicon_rotated"));

    // Off.
    run(h, "UCSICON OFF");
    CHECK(redSpot(h.screenshot()).count == 0);
    run(h, "UCSICON ON");
    CHECK(redSpot(h.screenshot()).count > 20);
}
