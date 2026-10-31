// Automated GUI tests: the real Gui is driven with queued mouse and keyboard events, without a
// window or a GPU (see gui_harness.h). They stand in for a person clicking through the program,
// so a panel that compiles but does nothing, or draws wrongly, fails here.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "gui.h"
#include "gui_harness.h"
#include "icons.h"
#include "tbcad/block_cmds/palette.h"
#include "tbcad/geom/revcloud.h"
#include "tbcad/layers/layer_states.h"
#include "tbcad/render/overlay.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/thai_words.h"
#include "tbcad/ui/toolbox.h"
#include "tbcad/xref/xref.h"
#include "window_link.h"

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

// The rarer Layers tools sit in the panel's "..." menu in the slim side column; `sub` names the
// sub-menu (Filters) the item is in.
bool layersMore(Harness& h, const char* item, const char* sub = nullptr) {
    if (!h.click("...", "Layers"))
        return false;
    h.frame(2);
    if (sub) {
        if (!h.click(sub, "##Popup"))
            return false;
        h.frame(2);
        const bool ok = h.click(item, "##Menu");
        h.frame(2);
        return ok;
    }
    const bool ok = h.click(item, "##Popup");
    h.frame(2);
    return ok;
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
    REQUIRE(layersMore(h, "Layers that are off", "Filters"));
    h.frame(2);
    CHECK(h.has("A-Doors", "Layers"));
    CHECK_FALSE(h.has("A-Walls", "Layers"));
    CHECK_FALSE(h.has("A-Notes", "Layers"));

    // Invert shows the others.
    REQUIRE(layersMore(h, "Invert filter"));
    CHECK_FALSE(h.has("A-Doors", "Layers"));
    CHECK(h.has("Defpoints", "Layers"));
    REQUIRE(layersMore(h, "Invert filter"));

    // Back to everything, then type a name into the filter box.
    REQUIRE(layersMore(h, "All layers", "Filters"));
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
    REQUIRE(layersMore(h, "New group..."));
    h.frame(2);
    REQUIRE(h.enterText("##name", "Frame", "Popup"));
    h.frame(2);
    const auto groups = layers::layerGroupNames(h.core().document());
    REQUIRE(groups.size() == 1);
    CHECK(groups[0] == "frame");

    // Pick the group in the Filters drop-down.
    REQUIRE(layersMore(h, "frame", "Filters"));
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

TEST_CASE("layers panel: column sorting and the property filter editor") {
    Harness h(1920, 1080);
    threeLayers(h);
    run(h, "-LAYER Off A-Doors");
    auto y = [&](const char* name) {
        const auto* it = h.find(name, "Layers");
        REQUIRE(it);
        return it->min.y;
    };
    CHECK(y("A-Doors") < y("A-Walls"));  // by name to start with
    // The Name header sits just above the first row (layer 0).
    const auto* zero = h.find("0", "Layers");
    REQUIRE(zero);
    const float headerX = zero->min.x + 20, headerY = zero->min.y - 8;
    h.click(headerX, headerY);  // again: Z-A
    h.frame(3);
    CHECK(y("A-Walls") < y("A-Doors"));
    h.click(headerX, headerY);  // and back
    h.frame(3);
    CHECK(y("A-Doors") < y("A-Walls"));

    // A property filter: the layers that are off, saved and applied.
    REQUIRE(layersMore(h, "New property filter..."));
    h.frame(2);
    REQUIRE(h.windowOpen("Layer Filter Properties"));
    REQUIRE(h.enterText("Filter name", "Off-ones", "Layer Filter Properties"));
    REQUIRE(h.click("On", "Layer Filter Properties"));
    h.frame(2);
    REQUIRE(h.click("Off", "Combo"));
    h.frame(2);
    REQUIRE(h.click("Save and apply", "Layer Filter Properties"));
    h.frame(3);
    const auto names = layers::layerFilterNames(h.core().document());
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "off-ones");
    CHECK(h.has("A-Doors", "Layers"));
    CHECK_FALSE(h.has("A-Walls", "Layers"));
}

TEST_CASE("quick properties open next to the pointer, or in the corner") {
    Harness h(1920, 1080);
    threeLayers(h);
    h.core().setSetting("ui.quickProps", "1");
    h.moveMouse(700, 250);
    h.core().selectAll();
    h.frame(3);
    const auto* general = h.find("General", "##quickprops");
    REQUIRE(general);
    // Below and to the right of the pointer, not on it.
    CHECK(general->min.x > 700);
    CHECK(general->min.x < 800);
    CHECK(general->min.y > 250);
    CHECK(general->min.y < 350);
    // The box stays put while the pointer moves on.
    h.moveMouse(300, 300);
    h.frame(2);
    REQUIRE(h.find("General", "##quickprops"));
    CHECK(h.find("General", "##quickprops")->min.x > 700);
    // QPLOCATION in the corner instead.
    h.core().setSetting("ui.quickProps.location", "corner");
    h.frame(3);
    REQUIRE(h.find("General", "##quickprops"));
    CHECK(h.find("General", "##quickprops")->min.x > 1000);
    CHECK(h.find("General", "##quickprops")->min.y < 300);
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

TEST_CASE("external references panel: tree, details, type switch, row menu and change balloon") {
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,0");
    h.core().pressEscape();
    const fs::path dir = scratchDir("xref2");
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
    REQUIRE(xref::xrefs(h.core().document()).size() == 1);
    const BlockId id = xref::xrefs(h.core().document())[0];
    const auto def = [&] { return h.core().document().block(id); };

    h.core().submitText("XREF");
    h.frame(3);
    REQUIRE(h.click("source", "External References"));
    h.frame(2);
    CHECK(h.has("Open", "External References"));
    REQUIRE(h.click("Make overlay", "External References"));
    h.frame(3);
    REQUIRE(def());
    CHECK(def()->xrefOverlay);
    CHECK(h.has("Make attachment", "External References"));

    REQUIRE(h.click("Tree", "External References"));
    h.frame(2);
    CHECK(h.has("source", "External References"));
    REQUIRE(h.rightClick("source", "External References"));
    h.frame(2);
    CHECK(h.has("Reference type"));
    CHECK(h.has("Detach"));
    h.click(900, 500);  // a click beside the menu closes it
    h.frame(2);
    CHECK_FALSE(h.has("Reference type"));

    // The file changes on disk: a balloon names it and reloads it on request.
    fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(10));
    h.core().tickXrefs(std::chrono::steady_clock::now() + std::chrono::hours(1));
    h.frame(2);
    CHECK(xref::state(h.core().document(), id, {}) == xref::State::Changed);
    REQUIRE(h.windowOpen("External reference changed"));
    CHECK(h.has("Xref changed"));  // the status bar marker
    REQUIRE(h.click("Reload source", "External reference changed"));
    h.frame(3);
    CHECK(xref::state(h.core().document(), id, {}) == xref::State::UpToDate);
    CHECK_FALSE(h.windowOpen("External reference changed"));
}

TEST_CASE("plot panel: options change the exported PDF") {
    Harness h(1920, 1200);
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
    // The drawing area of a 1920 x 1080 window: below the bars, left of the side panels, above
    // the command line (the Layers panel's red swatch must not count).
    for (int y = 85; y < 930 && y < img.height; ++y)
        for (int x = 0; x < 1500 && x < img.width; ++x) {
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
    o.x += ImGui::GetMainViewport()->WorkPos.x;  // the viewport starts after the bars
    o.y += ImGui::GetMainViewport()->WorkPos.y;

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

namespace {

// Where a world point is in the harness window (the viewport starts below the menu and tabs).
ImVec2 screenOf(Harness& h, Vec2 world) {
    const Vec2 s = h.core().camera().worldToScreen(world);
    return ImVec2(static_cast<float>(s.x) + ImGui::GetMainViewport()->WorkPos.x,
                  static_cast<float>(s.y) + ImGui::GetMainViewport()->WorkPos.y);
}

std::size_t entityCount(Harness& h) {
    std::size_t n = 0;
    h.core().document().forEachEntity([&](const Entity&) { ++n; });
    return n;
}

}  // namespace

TEST_CASE("viewport menu: idle and with a selection") {
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.core().submitText("100,100");
    h.core().submitText("400,100");
    h.core().pressEscape();
    h.frame(3);
    const ImVec2 mid = screenOf(h, {600, 300});

    // Nothing selected: Repeat, Select All, Paste, Zoom Extents; no edit commands.
    h.click(mid.x, mid.y, 1);
    h.frame(2);
    CHECK(h.has("Repeat LINE", "Popup"));
    CHECK(h.has("Select All", "Popup"));
    CHECK_FALSE(h.has("ERASE", "Popup"));
    CHECK(matchesGolden(h.windowShot("Popup"), "viewport_menu_idle"));
    REQUIRE(h.click("Select All", "Popup"));
    h.frame(3);
    CHECK(h.core().selection().size() == 1);

    // With a selection the edit commands appear; ERASE removes the line.
    h.click(mid.x, mid.y, 1);
    h.frame(2);
    CHECK(h.has("MOVE", "Popup"));
    CHECK(h.has("Deselect All", "Popup"));
    REQUIRE(h.click("ERASE", "Popup"));
    h.frame(3);
    CHECK(entityCount(h) == 0);
    CHECK(h.has("Repeat ERASE", "Popup") == false);  // the menu is closed again
}

TEST_CASE("viewport menu: during a command, options and snap overrides") {
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.core().submitText("100,100");
    h.core().submitText("400,100");
    h.core().pressEscape();
    h.core().setSetting("drafting.osnap.modes", "midpoint");  // endpoint only through the override
    h.core().submitText("LINE");  // a new line, waiting for its first point
    h.frame(3);
    const ImVec2 mid = screenOf(h, {600, 300});

    h.click(mid.x, mid.y, 1);
    h.frame(2);
    CHECK(h.has("Enter", "Popup"));
    CHECK(h.has("Cancel", "Popup"));
    CHECK(h.has("Snap Overrides", "Popup"));
    REQUIRE(h.click("Snap Overrides", "Popup"));
    h.frame(2);
    REQUIRE(h.click("Endpoint", "Menu"));
    h.frame(2);
    CHECK(h.core().setting("drafting.osnap.once").value_or("") == "endpoint");

    // The override serves one point: it snaps to the endpoint, then it is gone.
    const ImVec2 near400 = screenOf(h, {403, 104});
    h.click(near400.x, near400.y);
    h.frame(3);
    CHECK(h.core().setting("drafting.osnap.once").value_or("x").empty());
    h.core().submitText("700,300");
    h.core().pressEscape();
    h.frame(2);
    double startX = 0, startY = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* l = std::get_if<LineData>(&e.data))
            if (l->b.x == doctest::Approx(700) && l->b.y == doctest::Approx(300)) {
                startX = l->a.x;
                startY = l->a.y;
            }
    });
    CHECK(startX == doctest::Approx(400));
    CHECK(startY == doctest::Approx(100));

    // Cancel from the menu leaves the drawing as it is.
    h.core().submitText("LINE");
    h.frame(2);
    h.click(mid.x, mid.y, 1);
    h.frame(2);
    REQUIRE(h.click("Cancel", "Popup"));
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
}

TEST_CASE("viewport: Shift + right click opens the snap override menu at a point prompt") {
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.frame(2);
    const ImVec2 mid = screenOf(h, {600, 300});
    h.holdShift(true);
    h.click(mid.x, mid.y, 1);
    h.holdShift(false);
    h.frame(2);
    CHECK(h.has("Midpoint", "Popup"));
    CHECK(h.has("None", "Popup"));
    REQUIRE(h.click("Midpoint", "Popup"));
    h.frame(2);
    CHECK(h.core().setting("drafting.osnap.once").value_or("") == "midpoint");
    h.core().pressEscape();  // Esc drops the override with the command
    CHECK(h.core().setting("drafting.osnap.once").value_or("x").empty());
}

namespace {
std::string runningName(Harness& h) {
    const auto* p = h.core().runner().prompt();
    return p ? p->command : std::string();
}
}  // namespace

TEST_CASE("command bar: autocomplete picks, Tab completes, Up recalls history") {
    Harness h(1920, 1080);
    REQUIRE(h.click("##command", "Command"));

    // Typing narrows the list; Tab takes the first row, Enter runs it.
    h.type("cir");
    CHECK(h.has("##row", "##suggest"));
    // The list over the command line, from the input line's left end.
    const auto* input = h.find("##command", "Command");
    REQUIRE(input);
    CHECK(matchesGolden(crop(h.screenshot(), static_cast<int>(input->min.x) - 50,
                             static_cast<int>(input->max.y) - 230, 700, 230),
                        "command_suggest"));
    h.key(ImGuiKey_Tab);
    h.key(ImGuiKey_Enter);
    CHECK(h.core().runner().active());
    CHECK(runningName(h) == "CIRCLE");
    h.core().pressEscape();
    h.frame(2);

    // Down arrow moves the highlight, Enter then takes that row ("ci" -> CIRCLE, then the next).
    REQUIRE(h.click("##command", "Command"));
    h.type("li");
    h.key(ImGuiKey_DownArrow);
    h.key(ImGuiKey_Enter);
    h.frame(2);
    CHECK(h.core().runner().active());
    CHECK(runningName(h) != "LIST");  // the second row, not the first
    h.core().pressEscape();
    h.frame(2);

    // Up at an empty prompt brings back the latest line typed.
    h.command("LINE");
    h.core().pressEscape();
    h.frame(2);
    REQUIRE(h.click("##command", "Command"));
    h.key(ImGuiKey_UpArrow);
    h.key(ImGuiKey_Enter);
    h.frame(2);
    CHECK(h.core().runner().active());
    CHECK(runningName(h) == "LINE");
    h.core().pressEscape();

    // Esc closes the list without running anything.
    REQUIRE(h.click("##command", "Command"));
    h.type("ar");
    h.key(ImGuiKey_Escape);
    h.frame(2);
    CHECK_FALSE(h.has("##row", "##suggest"));
    CHECK_FALSE(h.core().runner().active());
}

TEST_CASE("levels panel: new, rename, elevation, show, lock, current, view and delete") {
    Harness h(1920, 1080);
    REQUIRE(h.click("Levels", "DockSpace"));  // the panel's tab
    h.frame(3);
    REQUIRE(h.windowOpen("Levels"));
    auto& doc = h.core().document();
    const std::size_t before = doc.levels().size();

    REQUIRE(h.click("New Level", "Levels"));
    h.frame(3);
    REQUIRE(doc.levels().size() == before + 1);
    const Level* upper = doc.levelByName("Level " + std::to_string(before + 1));
    REQUIRE(upper);
    CHECK(upper->elevation == doctest::Approx(3000));

    // Double-click the name to rename it.
    REQUIRE(h.doubleClickItem(upper->name, "Levels"));
    h.frame(2);
    REQUIRE(h.enterText("##rename", "Roof", "Levels"));
    h.frame(3);
    REQUIRE(doc.levelByName("Roof"));

    // Elevation field.
    REQUIRE(h.enterText("##elev", "6500", "Levels", 0));  // the highest level is the first row
    h.frame(3);
    CHECK(doc.levelByName("Roof")->elevation == doctest::Approx(6500));

    // A new level becomes current and cannot be hidden; make the ground level current first.
    REQUIRE(h.click("##cur", "Levels", 1));
    h.frame(3);
    CHECK(doc.header().currentLevel != doc.levelByName("Roof")->id);
    // Lock and hide the roof (the first row), then make it current.
    REQUIRE(h.click("##lck", "Levels", 0));
    h.frame(3);
    CHECK(doc.levelByName("Roof")->locked);
    REQUIRE(h.click("##vis", "Levels", 0));
    h.frame(3);
    CHECK_FALSE(doc.levelByName("Roof")->visible);
    REQUIRE(h.click("##cur", "Levels", 0));
    h.frame(3);
    CHECK(doc.header().currentLevel == doc.levelByName("Roof")->id);

    // The view of the other levels.
    REQUIRE(h.click("##view", "Levels"));
    REQUIRE(h.click("Hidden", "Combo"));
    h.frame(3);
    CHECK(doc.header().levelView == LevelView::HideOthers);

    // Delete the selected level (the roof was selected by clicking its name earlier).
    REQUIRE(h.click("Roof", "Levels"));
    REQUIRE(h.click("Delete", "Levels"));
    h.frame(3);
    CHECK_FALSE(doc.levelByName("Roof"));
    CHECK(doc.levels().size() == before);
}
TEST_CASE("object properties panel: layer, linetype, lineweight, transparency, scale") {
    Harness h(1920, 1080);
    run(h, "-LAYER New A-One");
    h.core().submitText("CIRCLE");
    h.core().submitText("0,0");
    h.core().submitText("10");
    h.core().selectAll();
    h.frame(2);
    REQUIRE(h.click("Object Properties", "DockSpace"));  // the panel's tab
    h.frame(3);
    const Entity* circle = nullptr;
    auto fetch = [&] {
        h.core().document().forEachEntity([&](const Entity& e) { circle = &e; });
        REQUIRE(circle);
        return circle;
    };
    // Every drop-down opens, shows ByLayer first and applies the pick as one undo step.
    REQUIRE(h.click("##layer", "Object Properties"));
    REQUIRE(h.click("A-One", "Combo"));
    h.frame(3);
    CHECK(fetch()->common.layer == layer(h, "A-One")->id);

    REQUIRE(h.click("##lt", "Object Properties"));
    CHECK(h.has("ByBlock", "Combo"));
    REQUIRE(h.click("Dashed", "Combo"));
    h.frame(3);
    CHECK(fetch()->common.linetype != kLinetypeByLayer);

    REQUIRE(h.click("##lw", "Object Properties"));
    REQUIRE(h.click("0.50", "Combo"));
    h.frame(3);
    CHECK(fetch()->common.lineweight == LineWeight::mm(0.5));

    REQUIRE(h.click("##tr", "Object Properties"));
    REQUIRE(h.click("50", "Combo"));
    h.frame(3);
    CHECK(fetch()->common.transparency == 50);

    REQUIRE(h.enterText("##ltscale", "2.5", "Object Properties"));
    h.frame(3);
    CHECK(fetch()->common.linetypeScale == doctest::Approx(2.5f));

    // One undo per edit: the transparency goes back first.
    h.command("U");
    h.frame(3);
    CHECK(fetch()->common.linetypeScale == doctest::Approx(1.0f));
    CHECK(fetch()->common.transparency == 50);
    CHECK(matchesGolden(h.windowShot("Object Properties"), "object_properties_circle"));
}

// Clicks every labelled widget of each panel once, with a drawing and a selection present. This
// finds crashes and ImGui misuse (the harness fails the test on those) in panels no scripted
// test covers yet; it checks no results.
TEST_CASE("every panel survives a click on each of its widgets") {
    struct P {
        const char* id;
        const char* tab;
    };
    for (const P& p :
         {P{"textstyles", "Text Styles"}, P{"dimstyles", "Dimension Styles"}, P{"blocks", "Blocks"},
          P{"hatch", "Hatch"}, P{"palettes", "Tool Palettes"}, P{"plotstyle", "Plot Styles"},
          P{"laywalk", "Layer Walk"}, P{"text_editor", "Text"},
          P{"mleaderstyles", "Multileader Style Manager"}, P{"tablestyles", "Table Style"},
          P{"mlstyles", "Multiline Style"}, P{"views", "View Manager"}, P{"ucsman", "UCS"},
          P{"vports", "Viewports"}, P{"pointstyle", "Point Style"}}) {
        CAPTURE(p.id);
        Harness h(1920, 1080);
        h.core().submitText("CIRCLE");
        h.core().submitText("0,0");
        h.core().submitText("10");
        h.core().selectAll();
        h.core().openPanel(p.id);
        h.frame(3);
        REQUIRE(h.has(p.tab, "DockSpace"));
        REQUIRE(h.click(p.tab, "DockSpace"));
        h.frame(3);
        // Collect the widgets first; the lists are rebuilt by every click.
        std::vector<std::string> labels;
        for (const auto& it : imgui_items::items())
            if (it.window.find(p.tab) != std::string::npos && !it.label.empty() &&
                it.label.rfind("##", 0) != 0 && it.label != p.tab)
                labels.push_back(it.label);
        for (const std::string& l : labels) {
            if (!h.has(l, p.tab))
                continue;
            h.click(l, p.tab);
            h.frame(2);
            h.key(ImGuiKey_Escape);  // closes a popup or combo it opened
            h.frame(1);
            if (!h.windowOpen(p.tab) && h.has(p.tab, "DockSpace"))
                h.click(p.tab, "DockSpace");
        }
    }
}

TEST_CASE("keyboard map: shortcuts, function keys and typing anywhere") {
    Harness h(1920, 1080);
    auto on = [&](const char* key, const char* fallback = "0") {
        return h.core().setting(key).value_or(fallback) == "1";
    };
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,0");
    h.core().pressEscape();
    h.frame(3);
    CHECK(entityCount(h) == 1);

    // Ctrl+A selects, Delete erases, Ctrl+Z brings it back, Ctrl+Y erases again.
    h.key(ImGuiKey_A, true);
    h.frame(2);
    CHECK(h.core().selection().size() == 1);
    h.key(ImGuiKey_Delete);
    h.frame(2);
    CHECK(entityCount(h) == 0);
    h.key(ImGuiKey_Z, true);
    h.frame(2);
    CHECK(entityCount(h) == 1);
    h.key(ImGuiKey_Y, true);
    h.frame(2);
    CHECK(entityCount(h) == 0);
    h.key(ImGuiKey_Z, true);
    h.frame(2);

    // F7 / F8 / F9 / F10 / F3 toggle the drafting aids; F8 and F10 exclude each other.
    h.key(ImGuiKey_F7);
    CHECK(on("view.grid"));
    h.key(ImGuiKey_F7);
    CHECK_FALSE(on("view.grid"));
    h.key(ImGuiKey_F9);
    CHECK(on("drafting.snap"));
    h.key(ImGuiKey_F8);
    CHECK(on("drafting.ortho"));
    h.key(ImGuiKey_F10);
    CHECK(on("drafting.polar"));
    CHECK_FALSE(on("drafting.ortho"));
    CHECK(on("drafting.osnap", "1"));
    h.key(ImGuiKey_F3);
    CHECK_FALSE(on("drafting.osnap", "1"));

    // Typing over the viewport goes to the command bar; Enter runs it.
    h.moveMouse(600, 400);
    h.type("circle");
    h.key(ImGuiKey_Enter);
    h.frame(2);
    CHECK(runningName(h) == "CIRCLE");
    // Esc cancels the command; a second Esc clears the selection.
    h.key(ImGuiKey_Escape);
    h.frame(2);
    CHECK_FALSE(h.core().runner().active());
    h.key(ImGuiKey_A, true);
    h.frame(2);
    REQUIRE(h.core().selection().size() == 1);
    h.key(ImGuiKey_Escape);
    h.frame(2);
    CHECK(h.core().selection().empty());

    // Enter at an empty prompt repeats the last command, Space too.
    h.moveMouse(600, 400);
    h.key(ImGuiKey_Enter);
    h.frame(2);
    CHECK(h.core().runner().active());
    h.key(ImGuiKey_Escape);
    h.frame(2);
    h.key(ImGuiKey_Space);
    h.frame(2);
    CHECK(h.core().runner().active());
    h.key(ImGuiKey_Escape);
}

TEST_CASE("Space after a typed command name runs it like Enter") {
    Harness h(1920, 1080);
    h.frame(2);
    REQUIRE(h.click("##command", "Command"));
    h.type("circle ");
    h.frame(2);
    CHECK(runningName(h) == "CIRCLE");
    h.key(ImGuiKey_Escape);
    h.frame(2);
}

TEST_CASE("command line focus: edit chords act on the text, Esc clears text then selection") {
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,0");
    h.core().pressEscape();
    h.frame(2);
    REQUIRE(entityCount(h) == 1);

    // Text typed in the command line: Ctrl+A selects the text, Ctrl+Z edits it; the drawing is
    // kept.
    REQUIRE(h.click("##command", "Command"));
    h.type("abc");
    h.key(ImGuiKey_A, true);
    h.frame(2);
    CHECK(h.core().selection().empty());
    h.key(ImGuiKey_Z, true);
    h.frame(2);
    CHECK(entityCount(h) == 1);

    // Empty command line: the same chords reach the drawing.
    h.key(ImGuiKey_Escape);
    h.frame(2);
    h.key(ImGuiKey_A, true);
    h.frame(2);
    CHECK(h.core().selection().size() == 1);

    // Esc closes the suggestion list, then clears the typed text, then clears the selection.
    REQUIRE(h.click("##command", "Command"));
    h.type("xyz");
    h.key(ImGuiKey_Escape);
    h.frame(2);
    CHECK(h.core().selection().size() == 1);
    h.key(ImGuiKey_Escape);
    h.frame(2);
    CHECK(h.core().selection().size() == 1);  // the first Esc closed the suggestion list, the
                                              // second cleared the text
    h.key(ImGuiKey_Escape);
    h.frame(2);
    CHECK(h.core().selection().empty());

    // Enter and Space repeat the last command from an empty line.
    h.command("CIRCLE");
    h.core().pressEscape();
    h.frame(2);
    h.moveMouse(600, 400);
    h.key(ImGuiKey_Enter);
    h.frame(2);
    CHECK(h.core().runner().active());
    CHECK(runningName(h) == "CIRCLE");
    h.core().pressEscape();
}

TEST_CASE("clean screen, command log and document tabs from the keyboard") {
    Harness h(1920, 1080);
    CHECK(h.has("Layers", "Layers"));
    h.key(ImGuiKey_F11);
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Layers"));
    h.key(ImGuiKey_F11);
    h.frame(3);
    CHECK(h.windowOpen("Layers"));

    // Ctrl+N opens a second drawing tab (an untouched drawing is reused, so touch it), Ctrl+Tab
    // switches, Ctrl+W closes.
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,0");
    h.core().pressEscape();
    h.frame(2);
    const std::size_t docs = h.core().documentCount();
    h.key(ImGuiKey_N, true);
    h.frame(3);
    CHECK(h.core().documentCount() == docs + 1);
    const std::size_t active = h.core().activeDocument();
    h.key(ImGuiKey_Tab, true);
    h.frame(3);
    CHECK(h.core().activeDocument() != active);
    h.key(ImGuiKey_Tab, true);  // back to the new, empty drawing
    h.frame(3);
    CHECK(h.core().activeDocument() == active);
    h.key(ImGuiKey_W, true);
    h.frame(3);
    CHECK(h.core().documentCount() == docs);  // the new empty one closed without asking
}

TEST_CASE("mouse in the viewport: draw with clicks, select by window and crossing, pan, zoom") {
    Harness h(1920, 1080);
    h.core().setSetting("drafting.osnap", "0");
    h.frame(3);
    // Draw two lines with the Line command and mouse clicks.
    h.core().submitText("LINE");
    const ImVec2 a = screenOf(h, {100, 100}), b = screenOf(h, {400, 100}),
                 c = screenOf(h, {400, 300});
    h.click(a.x, a.y);
    h.click(b.x, b.y);
    h.click(c.x, c.y);
    h.core().pressEscape();
    h.frame(2);
    REQUIRE(entityCount(h) == 2);
    // Zoom in on the drawing so the lines are long on screen (the grips are 8 pixels wide).
    h.core().zoomExtents();
    h.frame(20);
    h.scroll(700, 500, -3.0f);  // and back out a little, so the margins are on screen
    h.frame(30);

    // Window selection (drag right) takes only what lies fully inside.
    ImVec2 w0 = screenOf(h, {50, 50}), w1 = screenOf(h, {450, 150});  // second line sticks out
    h.drag(w0.x, w0.y, w1.x, w1.y);
    h.frame(2);
    CHECK(h.core().selection().size() == 1);
    // Crossing selection (drag left) takes anything the box touches.
    h.core().clearSelection();
    h.frame(2);
    h.drag(w1.x, w1.y, w0.x, w0.y);
    h.frame(2);
    CHECK(h.core().selection().size() == 2);
    h.core().clearSelection();

    // Click on an object selects it, Shift + click removes it again.
    // On the first line as drawn: the clicks land on whole pixels, which at the starting zoom
    // are several millimetres apart. A quarter of the way along, away from the midpoint grip.
    Vec2 first{}, second{};
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* ln = std::get_if<LineData>(&e.data); ln && first.x == 0 && first.y == 0) {
            first = {ln->a.x, ln->a.y};
            second = {ln->b.x, ln->b.y};
        }
    });
    const ImVec2 onLine =
        screenOf(h, {first.x + (second.x - first.x) * 0.27, first.y + (second.y - first.y) * 0.27});
    h.click(onLine.x, onLine.y);
    h.frame(2);
    CHECK(h.core().selection().size() == 1);
    h.frame(40);  // not a double click
    h.holdShift(true);
    h.frame(2);
    h.click(onLine.x, onLine.y);
    h.holdShift(false);
    h.frame(2);
    CHECK(h.core().selection().empty());

    // The wheel zooms about the cursor: the world point under it stays put.
    const ImVec2 cur = screenOf(h, {250, 100});
    const double before = h.core().camera().pixelSizeMm();
    h.moveMouse(cur.x, cur.y);
    h.scroll(cur.x, cur.y, 1.0f);
    h.frame(15);
    const double after = h.core().camera().pixelSizeMm();
    CHECK(after < before);  // zoomed in
    const ImVec2 now = screenOf(h, {250, 100});
    CHECK(now.x == doctest::Approx(cur.x).epsilon(0.01));
    CHECK(now.y == doctest::Approx(cur.y).epsilon(0.01));

    // Middle-drag pans: the drawing follows the pointer.
    h.drag(800, 500, 900, 560, 2);
    h.frame(3);
    const ImVec2 panned = screenOf(h, {250, 100});
    CHECK(panned.x == doctest::Approx(cur.x + 100).epsilon(0.03));
    CHECK(panned.y == doctest::Approx(cur.y + 60).epsilon(0.03));
}

TEST_CASE("F1 explains the running command, else opens the quick start") {
    Harness h(1920, 1080);
    h.key(ImGuiKey_F1);
    h.frame(3);
    CHECK(h.windowOpen("Quick Start"));
    h.key(ImGuiKey_Escape);  // Esc closes the dialog
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Quick Start"));

    h.core().submitText("CIRCLE");
    h.frame(2);
    h.moveMouse(600, 400);
    h.key(ImGuiKey_F1);
    h.frame(3);
    REQUIRE(h.windowOpen("Command Help"));
    CHECK(h.has("All commands...", "Command Help"));
#ifdef _WIN32
    // The golden picture is a Linux rendering; the font widths differ on Windows.
    CHECK(h.windowShot("Command Help").width > 150);
#else
    CHECK(matchesGolden(h.windowShot("Command Help"), "command_help_circle"));
#endif
    REQUIRE(h.click("Close", "Command Help"));
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Command Help"));
    CHECK(h.core().runner().active());  // the command itself keeps waiting
}

TEST_CASE("hover tooltip, right-click as Enter and the cursor HUD switch") {
    Harness h(1920, 1080);
    h.core().setSetting("drafting.osnap", "0");
    h.frame(3);
    // Typed points, so the line's length (shown in the tooltip picture) does not depend on how
    // wide the bars around the drawing are.
    h.command("LINE");
    h.core().submitText("100,100");
    h.core().submitText("400,100");
    h.core().pressEscape();
    h.core().zoomExtents();
    h.frame(20);
    h.scroll(700, 500, -3.0f);
    h.frame(30);
    REQUIRE(entityCount(h) == 1);
    // (a pixel up: the viewport starts on a fraction of a pixel, and the pick aperture is tight)
    ImVec2 onLine = screenOf(h, {180, 100});
    onLine.y -= 1.0f;

    // On by default, as AutoCAD's ROLLOVERTIPS; turned off, resting on the line shows nothing.
    // Set as text, like the SETTING command.
    h.core().setSetting("hover.tooltip", "0");
    h.moveMouse(onLine.x, onLine.y);
    h.frame(60);
    CHECK(h.windowShot("##Tooltip").empty());
    h.core().setSetting("hover.tooltip", "1");
    // The bars above the drawing can leave it on a fraction of a pixel that differs between
    // platforms, so find the row of pixels that is on the line first.
    for (const float dy : {0.0f, 1.0f, -1.0f, 2.0f, -2.0f}) {
        h.moveMouse(onLine.x + 3, onLine.y + dy);
        h.frame(60);
        if (!h.windowShot("##Tooltip").empty()) {
            onLine.y += dy;
            break;
        }
    }
    h.moveMouse(onLine.x + 300, onLine.y + 200);
    h.frame(3);
    h.moveMouse(onLine.x + 3, onLine.y);
    h.frame(20);
    CHECK(h.windowShot("##Tooltip").empty());  // not yet 600 ms
    h.frame(40);
    const Image tip = h.windowShot("##Tooltip");
    REQUIRE_FALSE(tip.empty());
    CHECK(matchesGolden(tip, "hover_tooltip_line"));
    h.moveMouse(onLine.x + 300, onLine.y + 200);
    h.frame(3);
    CHECK(h.windowShot("##Tooltip").empty());

    // ui.rightClick = enter: a right click at "Select objects" finishes it instead of a menu.
    h.core().setSetting("ui.rightClick", "enter");
    h.command("ERASE");
    h.click(onLine.x, onLine.y);
    h.frame(40);
    h.click(onLine.x + 300, onLine.y + 200, 1);
    h.frame(3);
    CHECK(entityCount(h) == 0);
    CHECK_FALSE(h.core().runner().active());
    CHECK(h.windowShot("##Popup").empty());

    // Right-click customization (SHORTCUTMENU): with nothing selected a right click repeats the
    // last command; while it runs, the menu or Enter as chosen.
    h.core().setSetting("ui.rightClick", "menu");
    h.core().setSetting("ui.rightClick.default", "repeat");
    h.core().setSetting("ui.rightClick.command", "menu");
    h.click(onLine.x + 300, onLine.y + 200, 1);
    h.frame(3);
    CHECK(h.core().runner().active());  // ERASE again
    CHECK(h.windowShot("##Popup").empty());
    h.click(onLine.x + 300, onLine.y + 200, 1);
    h.frame(3);
    CHECK(h.core().runner().active());  // the menu, not Enter
    h.key(ImGuiKey_Escape);             // closes the menu
    h.frame(3);
    h.core().pressEscape();  // ends ERASE
    h.frame(3);
    REQUIRE_FALSE(h.core().runner().active());
    h.core().setSetting("ui.rightClick.command", "enter");
    h.command("ERASE");
    h.frame(3);
    REQUIRE(h.core().runner().active());
    h.click(onLine.x + 300, onLine.y + 200, 1);
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    h.core().setSetting("ui.rightClick.default", "menu");
    h.core().setSetting("ui.rightClick.command", "menu");
}

TEST_CASE("file dialogs: Save As and Open round trip a drawing from the keyboard") {
    const fs::path dir = scratchDir("filedlg");
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,0");
    h.core().pressEscape();
    h.frame(3);
    REQUIRE(entityCount(h) == 1);

    // Ctrl+Shift+S opens the picker; pick the scratch folder, type a name, Save.
    h.key(ImGuiKey_S, true, true);
    h.frame(3);
    REQUIRE(h.windowOpen("File"));
    CHECK(h.enterText("Folder", dir.string(), "File"));
    h.frame(2);
    CHECK(h.enterText("File name", "round.tbcad", "File"));
    h.frame(2);
    if (h.windowOpen("File"))
        h.click("Save", "File");
    h.frame(5);
    CHECK_FALSE(h.windowOpen("File"));
    CHECK(fs::exists(dir / "round.tbcad"));
    CHECK(h.core().hasFile());

    // A fresh drawing is empty; Ctrl+O brings the saved one back.
    h.core().newDocument();
    h.frame(3);
    REQUIRE(entityCount(h) == 0);
    h.key(ImGuiKey_O, true);
    h.frame(3);
    REQUIRE(h.windowOpen("File"));
    CHECK(h.enterText("Folder", dir.string(), "File"));
    h.frame(2);
    CHECK(h.enterText("File name", "round.tbcad", "File"));
    h.frame(2);
    if (h.windowOpen("File"))
        h.click("Open", "File");
    for (int i = 0; i < 30 && entityCount(h) == 0; ++i)
        h.frame(5);
    CHECK_FALSE(h.windowOpen("File"));
    CHECK(entityCount(h) == 1);
}

TEST_CASE("dynamic input: Tab fields lock distance and angle for the next point") {
    Harness h(1920, 1080);
    h.core().setSetting("drafting.osnap", "0");
    h.core().setSetting("drafting.ortho", "0");
    h.core().setSetting("drafting.polar", "0");
    h.frame(3);
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().zoomExtents();
    h.frame(5);
    const ImVec2 c = screenOf(h, {0, 0});
    h.moveMouse(c.x + 200, c.y - 100);
    h.frame(5);
    // The HUD shows both fields.
    h.key(ImGuiKey_Tab);
    h.frame(3);
    h.type("50");
    h.key(ImGuiKey_Enter);
    h.frame(3);
    h.type("90");
    h.key(ImGuiKey_Enter);
    h.frame(5);
    h.core().pressEscape();
    h.frame(3);
    REQUIRE(entityCount(h) == 1);
    h.core().document().forEachEntity([&](const Entity& e) {
        const auto* ln = std::get_if<LineData>(&e.data);
        REQUIRE(ln);
        CHECK(ln->b.x == doctest::Approx(0).epsilon(1e-6).scale(1));
        CHECK(ln->b.y == doctest::Approx(50).epsilon(1e-6));
    });
}

TEST_CASE("viewport menu: Recent Input answers the prompt with a value typed before") {
    Harness h(1920, 1080);
    h.frame(3);
    h.command("CIRCLE");
    h.command("0,0");
    h.command("25");
    h.frame(3);
    REQUIRE(entityCount(h) == 1);
    h.command("CIRCLE");
    h.command("100,0");  // now at the radius prompt
    const ImVec2 spot = screenOf(h, {600, 300});
    h.click(spot.x, spot.y, 1);
    h.frame(2);
    REQUIRE(h.click("Recent Input", "Popup"));
    h.frame(2);
    CHECK(h.has("25", "Menu"));
    CHECK(h.has("100,0", "Menu"));
    REQUIRE(h.click("25", "Menu"));
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    CHECK(entityCount(h) == 2);
    int radius25 = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* c = std::get_if<CircleData>(&e.data))
            radius25 += c->radius == doctest::Approx(25);
    });
    CHECK(radius25 == 2);
}

TEST_CASE("viewport menu: selection items fit the picked object and run") {
    Harness h(1920, 1080);
    threeLayers(h);  // three lines on three layers, current layer A-Notes
    run(h, "-LAYER Make 0");
    h.core().submitText("CIRCLE");
    h.core().submitText("50,50");
    h.core().submitText("5");
    h.frame(3);
    REQUIRE(entityCount(h) == 4);
    const ImVec2 spot = screenOf(h, {600, 300});
    auto menu = [&] {
        h.click(spot.x, spot.y, 1);
        h.frame(2);
    };

    // A lone circle: the curve edits are offered; the polyline and text ones are not.
    h.core().document().forEachEntity([&](const Entity& e) {
        if (std::holds_alternative<CircleData>(e.data))
            h.core().setSelection({e.common.id});
    });
    h.frame(2);
    REQUIRE(h.core().selection().size() == 1);
    menu();
    CHECK(h.has("Offset", "Popup"));
    CHECK(h.has("Trim", "Popup"));
    CHECK_FALSE(h.has("Edit Text", "Popup"));
    CHECK_FALSE(h.has("Edit Polyline", "Popup"));
    CHECK(h.has("Select Similar", "Popup"));

    // Isolate Layer from the menu locks and fades the other layers (LAYISO's default).
    REQUIRE(h.click("Isolate Layer", "Popup"));
    h.frame(3);
    h.core().pressEscape();
    CHECK(layer(h, "A-Walls")->locked);
    CHECK(layer(h, "A-Doors")->locked);
    CHECK(layer(h, "A-Walls")->on);
    CHECK_FALSE(layer(h, "0")->locked);
}

TEST_CASE("layout tabs: add, switch, rename, delete and back to Model") {
    Harness h(1920, 1080);
    h.frame(3);
    REQUIRE(h.has("Model", "##layouttabs"));
    CHECK_FALSE(h.core().inPaperSpace());

    // "+" asks for a name, OK makes a sheet and shows it.
    REQUIRE(h.click("+", "##layouttabs"));
    h.frame(3);
    REQUIRE(h.windowOpen("Popup"));
    CHECK(h.enterText("##n", "Plan A", "Popup"));
    h.frame(5);
    CHECK(h.has("Plan A", "##layouttabs"));
    CHECK(h.core().inPaperSpace());
    CHECK(h.core().activeLayout() == "Plan A");

    // Model tab goes back to model space; the layout tab returns to the sheet.
    REQUIRE(h.click("Model", "##layouttabs"));
    h.frame(3);
    CHECK_FALSE(h.core().inPaperSpace());
    REQUIRE(h.click("Plan A", "##layouttabs"));
    h.frame(3);
    CHECK(h.core().inPaperSpace());

    // Right click on the tab: Rename...
    REQUIRE(h.rightClick("Plan A", "##layouttabs"));
    h.frame(2);
    REQUIRE(h.click("Rename...", "Popup"));
    h.frame(3);
    REQUIRE(h.windowOpen("Popup"));
    CHECK(h.enterText("##n", "Plan B", "Popup"));
    h.frame(5);
    CHECK(h.has("Plan B", "##layouttabs"));
    CHECK_FALSE(h.has("Plan A", "##layouttabs"));
    CHECK(h.core().activeLayout() == "Plan B");

    // ... and Delete drops the sheet and returns to Model.
    REQUIRE(h.rightClick("Plan B", "##layouttabs"));
    h.frame(2);
    REQUIRE(h.click("Delete", "Popup"));
    h.frame(5);
    CHECK_FALSE(h.has("Plan B", "##layouttabs"));
    CHECK_FALSE(h.core().inPaperSpace());
}

TEST_CASE("layout tabs: copy and move from the right-click menu") {
    Harness h(1920, 1080);
    h.frame(3);
    for (const char* n : {"One", "Two"}) {
        REQUIRE(h.click("+", "##layouttabs"));
        h.frame(3);
        CHECK(h.enterText("##n", n, "Popup"));
        h.frame(5);
    }
    auto order = [&] {
        std::string s;
        for (const auto& n : h.core().layoutNames())
            s += n + ",";
        return s;
    };
    CHECK(order() == "Layout1,Layout2,One,Two,");  // after the built-in drawing's two
    REQUIRE(h.rightClick("Two", "##layouttabs"));
    h.frame(2);
    REQUIRE(h.click("Move left", "Popup"));
    h.frame(5);
    CHECK(order() == "Layout1,Layout2,Two,One,");
    REQUIRE(h.rightClick("Two", "##layouttabs"));
    h.frame(2);
    REQUIRE(h.click("Copy", "Popup"));
    h.frame(5);
    CHECK(h.core().layoutNames().size() == 5);
    CHECK(h.has("Two (2)", "##layouttabs"));
}

TEST_CASE("layout tabs: Ctrl+PageDown and Ctrl+PageUp step Model and the layouts") {
    Harness h(1920, 1080);
    h.frame(3);
    REQUIRE(h.core().layoutNames().size() >= 1);
    CHECK_FALSE(h.core().inPaperSpace());
    h.key(ImGuiKey_PageDown, true);
    h.frame(3);
    CHECK(h.core().activeLayout() == h.core().layoutNames().front());
    h.key(ImGuiKey_PageUp, true);
    h.frame(3);
    CHECK_FALSE(h.core().inPaperSpace());
}

TEST_CASE("drawing tabs: Ctrl+Tab and Ctrl+Shift+Tab switch") {
    Harness h(1920, 1080);
    h.frame(3);
    run(h, "LINE 0,0 100,0");
    h.core().pressEscape();
    h.core().newDocument();
    h.frame(3);
    REQUIRE(h.core().documentCount() == 2);
    CHECK(h.core().activeDocument() == 1);
    h.key(ImGuiKey_Tab, true, true);
    h.frame(3);
    CHECK(h.core().activeDocument() == 0);
    h.key(ImGuiKey_Tab, true);
    h.frame(3);
    CHECK(h.core().activeDocument() == 1);
}

TEST_CASE("grips: click-move a vertex, and the hover menu adds a vertex") {
    Harness h(1920, 1080);
    h.core().setSetting("drafting.osnap", "0");
    h.frame(3);
    h.core().submitText("PLINE");
    h.core().submitText("0,0");
    h.core().submitText("200,0");
    h.core().submitText("200,200");
    h.core().submitText("");
    h.core().zoomExtents();
    h.frame(20);
    h.scroll(700, 500, -3.0f);
    h.frame(30);
    auto verts = [&] {
        std::size_t n = 0;
        h.core().document().forEachEntity([&](const Entity& e) {
            if (const auto* p = std::get_if<PolylineData>(&e.data))
                n = p->v.size();
        });
        return n;
    };
    auto vertex = [&](std::size_t i) {
        Vec2 out{};
        h.core().document().forEachEntity([&](const Entity& e) {
            if (const auto* p = std::get_if<PolylineData>(&e.data))
                out = p->v[i].p;
        });
        return out;
    };
    REQUIRE(verts() == 3);
    h.core().selectAll();
    h.frame(3);

    // A click on a vertex grip, then a click elsewhere, moves the vertex.
    const ImVec2 grip = screenOf(h, {200, 0});
    h.click(grip.x, grip.y);
    h.frame(3);
    REQUIRE(h.core().runner().active());
    const ImVec2 to = screenOf(h, {300, 0});
    h.click(to.x, to.y);
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    CHECK(vertex(1).x == doctest::Approx(300).epsilon(0.02));
    CHECK(vertex(1).y == doctest::Approx(0).scale(100).epsilon(0.02));
    h.core().selectAll();
    h.frame(3);

    // Hovering the middle vertex for a moment offers its actions.
    const ImVec2 corner = screenOf(h, {300, 0});
    h.moveMouse(corner.x, corner.y);
    h.frame(40);
    REQUIRE(h.windowOpen("##gripmenu"));
    CHECK(h.has("Add vertex", "##gripmenu"));
    CHECK(h.has("Remove vertex", "##gripmenu"));
    REQUIRE(h.click("Remove vertex", "##gripmenu"));
    h.frame(5);
    h.core().pressEscape();
    CHECK(verts() == 2);
}

TEST_CASE("revision cloud: arc length in Object Properties, a corner grip lays the arcs again") {
    Harness h(1920, 1080);
    h.core().setSetting("drafting.osnap", "0");
    h.frame(3);
    for (const char* s : {"REVCLOUD", "Arclength", "25", "0,0", "100,0", "100,50", "0,50", ""})
        h.core().submitText(s);
    h.core().zoomExtents();
    h.frame(20);
    h.scroll(700, 500, -3.0f);
    h.frame(30);
    auto cloud = [&] {
        std::optional<geom::CloudShape> s;
        std::size_t n = 0;
        h.core().document().forEachEntity([&](const Entity& e) {
            s = geom::cloudShape(e);
            if (const auto* p = std::get_if<PolylineData>(&e.data))
                n = p->v.size();
        });
        return std::make_pair(s, n);
    };
    REQUIRE(cloud().first);
    REQUIRE(cloud().second == 12);
    h.core().selectAll();
    h.frame(2);
    REQUIRE(h.click("Object Properties", "DockSpace"));
    h.frame(3);
    // The first number row of a cloud is its arc length (labels are plain text, not items).
    REQUIRE(h.has("##v", "Object Properties"));
    REQUIRE(h.enterText("##v", "10", "Object Properties"));
    h.frame(3);
    CHECK(cloud().second == 30);
    CHECK(cloud().first->arcLength == doctest::Approx(10));
    REQUIRE(h.click("##flag", "Object Properties"));  // Arcs inward: the first flag row
    h.frame(3);
    CHECK(cloud().first->inward);

    // Click the top right corner grip, then a point further right: the arcs follow the outline.
    h.core().selectAll();
    h.frame(3);
    const ImVec2 grip = screenOf(h, {100, 50});
    h.click(grip.x, grip.y);
    h.frame(3);
    REQUIRE(h.core().runner().active());
    const ImVec2 to = screenOf(h, {150, 50});
    h.click(to.x, to.y);
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    const auto [shape, n] = cloud();
    REQUIRE(shape);
    REQUIRE(shape->outline.size() == 4);
    CHECK(shape->outline[2].x == doctest::Approx(150).epsilon(0.02));
    CHECK(shape->arcLength == doctest::Approx(10));
    CHECK(n > 30);
    CHECK(h.imguiErrors().empty());
}

TEST_CASE("every menu item can be clicked without trouble") {
    std::vector<std::string> tops;
    {
        Harness h(1920, 1080);
        h.frame(3);
        for (const auto& it : imgui_items::items())
            if (it.window.find("MainMenuBar") != std::string::npos && !it.label.empty() &&
                it.label.rfind("##", 0) != 0 &&  // not the search box
                it.label != "Update")            // the version corner's button, not a menu
                tops.push_back(it.label);
    }
    REQUIRE(tops.size() >= 5);
    for (const std::string& top : tops) {
        CAPTURE(top);
        // The items of this menu, found with it open.
        std::vector<std::string> items;
        {
            Harness h(1920, 1080);
            h.frame(3);
            REQUIRE(h.click(top, "MainMenuBar"));
            h.frame(3);
            for (const auto& it : imgui_items::items())
                if (it.window.find("##Menu") != std::string::npos && !it.label.empty() &&
                    it.label.rfind("##", 0) != 0)
                    items.push_back(it.label);
        }
        CHECK_FALSE(items.empty());
        for (const std::string& item : items) {
            CAPTURE(item);
            if (item == "Exit")
                continue;
            Harness h(1920, 1080);
            h.core().submitText("CIRCLE");
            h.core().submitText("0,0");
            h.core().submitText("10");
            h.core().selectAll();
            h.frame(3);
            REQUIRE(h.click(top, "MainMenuBar"));
            h.frame(3);
            if (!h.has(item, "##Menu"))
                continue;
            h.click(item, "##Menu");
            h.frame(3);
            h.key(ImGuiKey_Escape);
            h.frame(2);
            h.core().pressEscape();
            h.key(ImGuiKey_Escape);
            h.frame(2);
        }
    }
}

TEST_CASE("toolbox: every button on the first tab starts its command") {
    int total = 0;
    {
        Harness h(1920, 1080);
        h.frame(3);
        while (h.has("##tool", "##toolbox") && h.find("##tool", "##toolbox", total))
            ++total;
    }
    REQUIRE(total >= 20);
    int started = 0;
    for (int i = 0; i < total; ++i) {
        CAPTURE(i);
        Harness h(1920, 1080);
        h.core().submitText("CIRCLE");
        h.core().submitText("0,0");
        h.core().submitText("10");
        h.frame(3);
        REQUIRE(h.click("##tool", "##toolbox", i));
        h.frame(4);
        if (h.core().runner().active()) {
            ++started;
            h.core().pressEscape();
        }
        h.key(ImGuiKey_Escape);  // a file picker or dialog
        h.frame(2);
    }
    // The Draw tab's tools all run commands that wait for input after a click.
    CHECK(started >= 18);
}

TEST_CASE("text editor: TEXT is typed in the box on the drawing, Thai included") {
    Harness h(1920, 1080);
    h.frame(3);
    h.core().submitText("TEXT");
    h.core().submitText("0,0");
    h.core().submitText("5");
    h.core().submitText("0");
    h.frame(8);
    REQUIRE(h.windowOpen("##textedit"));
    h.type("Room 101 \xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5");
    h.frame(3);
    h.key(ImGuiKey_Enter);
    h.frame(5);
    CHECK_FALSE(h.windowOpen("##textedit"));
    REQUIRE(entityCount(h) == 1);
    h.core().document().forEachEntity([&](const Entity& e) {
        const auto* t = std::get_if<Indirect<TextData>>(&e.data);
        REQUIRE(t);
        CHECK((*t)->utf8 ==
              "Room 101 \xE0\xB8\xAA\xE0\xB8\xA7\xE0\xB8\xB1\xE0\xB8\xAA\xE0\xB8\x94\xE0\xB8\xB5");
        CHECK((*t)->height == doctest::Approx(5));
    });
}

// Thai clusters in the text box: Left and Delete step over a consonant with its tone mark, while
// Backspace takes off one typed character (the tone mark) as in Windows.
static std::string typedInTextBox(const std::vector<ImGuiKey>& keys) {
    Harness h(1920, 1080);
    h.frame(3);
    h.core().submitText("TEXT");
    h.core().submitText("0,0");
    h.core().submitText("5");
    h.core().submitText("0");
    h.frame(8);
    REQUIRE(h.windowOpen("##textedit"));
    h.type("\xE0\xB8\x81\xE0\xB9\x89\xE0\xB8\x82");  // ก ้ ข
    h.frame(3);
    for (ImGuiKey k : keys) {
        h.key(k);
        h.frame(2);
    }
    h.type("x");
    h.frame(3);
    h.key(ImGuiKey_Enter);
    h.frame(5);
    std::string out;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* t = std::get_if<Indirect<TextData>>(&e.data))
            out = (*t)->utf8;
    });
    return out;
}

TEST_CASE("text box: Left steps over a whole Thai cluster") {
    // Left, Left: before the consonant with its tone mark, not between the two.
    CHECK(typedInTextBox({ImGuiKey_LeftArrow, ImGuiKey_LeftArrow}) ==
          "x\xE0\xB8\x81\xE0\xB9\x89\xE0\xB8\x82");
}

TEST_CASE("text box: Delete removes a whole Thai cluster, Backspace one character") {
    CHECK(typedInTextBox({ImGuiKey_Home, ImGuiKey_Delete}) == "x\xE0\xB8\x82");
    CHECK(typedInTextBox({ImGuiKey_LeftArrow, ImGuiKey_Backspace}) == "\xE0\xB8\x81x\xE0\xB8\x82");
}

TEST_CASE("text editor: the MTEXT formatting bar wraps the selection and inserts symbols") {
    Harness h(1920, 1080);
    h.frame(3);
    h.core().submitText("MTEXT");
    h.core().submitText("0,0");
    h.core().submitText("100,-40");
    h.frame(8);
    REQUIRE(h.windowOpen("##textedit"));
    h.type("Hello");
    h.frame(2);
    h.key(ImGuiKey_A, true);  // select all, then Bold from the bar
    h.frame(2);
    REQUIRE(h.click("B", "##textedit"));
    h.frame(4);
    h.key(ImGuiKey_End, true);  // the box has the keyboard again: go to the end
    h.frame(2);
    REQUIRE(h.click("@ Symbol", "##textedit"));
    h.frame(3);
    REQUIRE(h.click("Degree  \xC2\xB0"));
    h.frame(4);
    CHECK(h.has("Bullets and numbering", "##textedit"));
    CHECK(h.has("Columns", "##textedit"));
    h.key(ImGuiKey_Enter, true);  // Ctrl+Enter accepts
    h.frame(5);
    CHECK_FALSE(h.windowOpen("##textedit"));
    REQUIRE(entityCount(h) == 1);
    h.core().document().forEachEntity([&](const Entity& e) {
        const auto* m = std::get_if<Indirect<MTextData>>(&e.data);
        REQUIRE(m);
        CHECK((*m)->utf8 == "{\\b1;Hello}\xC2\xB0");
    });
}

TEST_CASE("object properties: annotation section, scales and *varies* with a mixed selection") {
    Harness h(1920, 1080);
    run(h, "-LAYER New A-One");
    run(h, "-LAYER Make 0");
    h.core().submitText("TEXT");
    h.core().submitText("0,0");
    h.core().submitText("2.5");
    h.core().submitText("0");
    h.frame(6);
    h.type("Note");
    h.key(ImGuiKey_Enter);
    h.frame(4);
    REQUIRE(entityCount(h) == 1);
    h.core().selectAll();
    h.frame(2);
    REQUIRE(h.click("Object Properties", "DockSpace"));
    h.frame(3);
    auto textAnno = [&] {
        bool on = false;
        h.core().document().forEachEntity([&](const Entity& e) { on = e.common.annotative; });
        return on;
    };

    // The section is there for text; the checkbox makes it annotative in one undo step.
    REQUIRE(h.has("Annotative", "Object Properties"));
    CHECK_FALSE(textAnno());
    REQUIRE(h.click("Annotative", "Object Properties"));
    h.frame(3);
    h.core().pressEscape();
    CHECK(textAnno());
    CHECK(h.has("Change...", "Object Properties"));
    CHECK(matchesGolden(h.windowShot("Object Properties"), "object_properties_text_annotative"));
    h.command("U");
    h.frame(3);
    CHECK_FALSE(textAnno());

    // A circle on another layer joins the selection: the layer reads *varies*, geometry hides.
    h.core().submitText("-LAYER Make A-One");
    h.core().pressEscape();
    h.core().submitText("CIRCLE");
    h.core().submitText("50,0");
    h.core().submitText("5");
    h.core().selectAll();
    h.frame(4);
    REQUIRE(h.has("##layer", "Object Properties"));
    CHECK_FALSE(h.has("##v", "Object Properties"));  // no geometry fields for two kinds
    // Narrowing to the circle shows its geometry again.
    REQUIRE(h.click("##type", "Object Properties"));
    REQUIRE(h.click("Circle (1)", "Combo"));
    h.frame(3);
    CHECK(h.has("##v", "Object Properties"));
}

TEST_CASE("status bar: toggles flip their setting, ORTHO and POLAR exclude each other") {
    Harness h(1920, 1080);
    h.frame(3);
    auto on = [&](const char* key) {
        const auto v = h.core().setting(key);
        return v && (*v == "1" || *v == "true");
    };
    struct T {
        const char* label;
        const char* key;
    };
    for (const T& t : {T{"SNAP", "drafting.snap"}, T{"GRID", "view.grid"},
                       T{"OTRACK", "drafting.otrack"}, T{"SELCYC", "ui.selectionCycling"}}) {
        CAPTURE(t.label);
        REQUIRE(h.has(t.label, "##statusbar"));
        const bool before = on(t.key);
        REQUIRE(h.click(t.label, "##statusbar"));
        h.frame(2);
        CHECK(on(t.key) == !before);
        REQUIRE(h.click(t.label, "##statusbar"));
        h.frame(2);
        CHECK(on(t.key) == before);
    }
    REQUIRE(h.click("ORTHO", "##statusbar"));
    h.frame(2);
    CHECK(on("drafting.ortho"));
    REQUIRE(h.click("POLAR", "##statusbar"));
    h.frame(2);
    CHECK(on("drafting.polar"));
    CHECK_FALSE(on("drafting.ortho"));

    // Right click on a drafting toggle opens the drafting settings.
    REQUIRE(h.rightClick("POLAR", "##statusbar"));
    h.frame(2);
    REQUIRE(h.click("Drafting settings...", "Popup"));
    h.frame(3);
    CHECK(h.windowOpen("Settings"));

    h.key(ImGuiKey_Escape);
    h.frame(2);

    // The same menu also holds the common increments (Q167).
    REQUIRE(h.rightClick("POLAR", "##statusbar"));
    h.frame(2);
    REQUIRE(h.click("30 degrees", "Popup"));
    h.frame(2);
    CHECK(h.core().setting("drafting.polar.increment").value_or("") == "30");
}

TEST_CASE("SELECTIONANNODISPLAY: a selected annotative object shows its other scales") {
    Harness h(1920, 1080);
    h.core().setSetting("view.crosshair", "off");
    h.frame(3);
    h.core().submitText("TEXT");
    h.core().submitText("0,0");
    h.core().submitText("10");
    h.core().submitText("0");
    h.frame(6);
    h.type("Note");
    h.key(ImGuiKey_Enter);
    h.frame(4);
    REQUIRE(entityCount(h) == 1);
    run(h, "SELECTIONANNODISPLAY 0");
    h.core().selectAll();
    h.core().submitText("ANNOTATIVE");
    h.core().submitText("Scales");
    h.core().submitText("1,2,4");
    h.core().pressEscape();
    h.core().submitText("CANNOSCALE");
    h.core().submitText("1");
    h.core().zoomExtents();
    h.frame(30);
    h.scroll(700, 500, -3.0f);
    h.frame(30);
    h.core().selectAll();
    h.moveMouse(1300, 800);
    h.frame(5);
    REQUIRE(h.core().selection().size() == 1);
    // The overlay (drawn on top of the scene) gains the ghost outlines when the variable is on.
    auto overlayVertices = [&] {
        render::Overlay overlay;
        h.core().buildOverlay(overlay);
        return overlay.build(h.core().camera()).vertices.size();
    };
    const std::size_t off = overlayVertices();
    run(h, "SELECTIONANNODISPLAY 1");
    h.core().selectAll();
    h.frame(5);
    REQUIRE(h.core().selection().size() == 1);
    const std::size_t on = overlayVertices();
    CHECK(on > off + 100);
    // Nothing selected, nothing to show.
    h.core().clearSelection();
    h.frame(2);
    CHECK(overlayVertices() < on);
}

TEST_CASE("layer states manager: new, restore chosen properties, rename, delete") {
    Harness h(1920, 1080);
    threeLayers(h);  // A-Walls, A-Doors, A-Notes on; current layer A-Notes
    REQUIRE(layersMore(h, "Layer states..."));
    h.frame(3);
    REQUIRE(h.windowOpen("Layer States Manager"));

    // New saves the layers as they are now.
    REQUIRE(h.enterText("##mgrname", "all-on", "Layer States Manager"));
    h.frame(2);
    REQUIRE(h.click("New", "Layer States Manager"));
    h.frame(3);
    CHECK(h.has("all-on", "Layer States Manager"));
    CHECK(layers::layerStateNames(h.core().document()).size() == 1);

    // Change things, then restore only Visibility: A-Doors comes back on, the lock stays.
    h.core().submitText("-LAYER Off A-Doors");
    h.core().pressEscape();
    h.core().submitText("-LAYER Lock A-Walls");
    h.core().pressEscape();
    h.frame(3);
    REQUIRE_FALSE(layer(h, "A-Doors")->on);
    REQUIRE(layer(h, "A-Walls")->locked);
    REQUIRE(h.windowOpen("Layer States Manager"));
    REQUIRE(h.click("all-on", "Layer States Manager"));
    REQUIRE(h.click("Locked", "Layer States Manager"));  // untick the lock part
    REQUIRE(h.click("Plot", "Layer States Manager"));
    REQUIRE(h.click("Colour, linetype, lineweight", "Layer States Manager"));
    REQUIRE(h.click("Current layer", "Layer States Manager"));
    h.frame(2);
    REQUIRE(h.click("Restore", "Layer States Manager"));
    h.frame(3);
    CHECK(layer(h, "A-Doors")->on);
    CHECK(layer(h, "A-Walls")->locked);
    CHECK(matchesGolden(h.windowShot("Layer States Manager"), "layer_states_manager"));

    // Rename, then Delete.
    REQUIRE(h.windowOpen("Layer States Manager"));
    REQUIRE(h.enterText("##mgrname", "base", "Layer States Manager"));
    h.frame(2);
    REQUIRE(h.click("Rename", "Layer States Manager"));
    h.frame(3);
    CHECK(h.has("base", "Layer States Manager"));
    CHECK_FALSE(h.has("all-on", "Layer States Manager"));
    REQUIRE(h.click("Delete", "Layer States Manager"));
    h.frame(3);
    CHECK(layers::layerStateNames(h.core().document()).empty());
    REQUIRE(h.click("Close", "Layer States Manager"));
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Layer States Manager"));
}

TEST_CASE("dimension styles panel golden") {
    Harness h(1920, 1080);
    h.core().openPanel("dimstyles");
    h.frame(3);
    h.click("Dimension Styles", "DockSpace");
    h.frame(3);
    // Panel selection is process-wide state; pin it so the picture does not depend on test order.
    h.click("Standard  (current)", "Dimension Styles");
    h.frame(3);
    CHECK(matchesGolden(h.windowShot("Dimension Styles"), "dimstyles_panel"));
}

TEST_CASE("hatch panel golden") {
    Harness h(1920, 1080);
    h.core().openPanel("hatch");
    h.frame(3);
    h.click("Hatch", "DockSpace");
    h.frame(3);
    CHECK(matchesGolden(h.windowShot("Hatch"), "hatch_panel"));
}

TEST_CASE("layer translator: rename, merge, one undo step") {
    Harness h(1920, 1080);
    threeLayers(h);
    REQUIRE(layersMore(h, "Translate..."));
    h.frame(3);
    REQUIRE(h.windowOpen("Layer Translator"));

    // Rename A-Doors, merge A-Notes into A-Walls; the filter narrows the list to one box.
    REQUIRE(h.enterText("##trfilter", "A-Doors", "Layer Translator"));
    h.frame(2);
    REQUIRE(h.enterText("##to", "B-Doors", "Layer Translator"));
    h.frame(2);
    REQUIRE(h.enterText("##trfilter", "A-Notes", "Layer Translator"));
    h.frame(2);
    REQUIRE(h.enterText("##to", "A-Walls", "Layer Translator"));
    h.frame(2);
    const std::size_t before = h.core().document().layers().size();
    REQUIRE(h.click("Translate", "Layer Translator"));
    h.frame(3);
    CHECK(layer(h, "B-Doors") != nullptr);
    CHECK(layer(h, "A-Doors") == nullptr);
    CHECK(layer(h, "A-Notes") == nullptr);
    CHECK(layer(h, "A-Walls") != nullptr);
    CHECK(h.core().document().layers().size() == before - 1);

    // One undo brings every name back.
    REQUIRE(h.click("Close", "Layer Translator"));
    h.frame(3);
    h.command("U");
    h.frame(3);
    CHECK(layer(h, "A-Doors") != nullptr);
    CHECK(layer(h, "A-Notes") != nullptr);
    CHECK(layer(h, "B-Doors") == nullptr);
}

TEST_CASE("define attribute panel: focus on open, Enter accepts, Esc cancels") {
    {
        Harness h(1920, 1080);
        h.core().openPanel("attdef");
        h.frame(4);
        REQUIRE(h.windowOpen("Define Attribute"));
        h.type("PART");  // goes to the Tag box: it has focus without a click
        h.frame(2);
        h.key(ImGuiKey_Enter);
        h.frame(4);
        CHECK(h.core().setting("tmp.attdef.tag").value_or("") == "PART");
        CHECK_FALSE(h.windowOpen("Define Attribute"));
    }
    {
        Harness h(1920, 1080);
        h.core().openPanel("attdef");
        h.frame(4);
        REQUIRE(h.windowOpen("Define Attribute"));
        h.key(ImGuiKey_Escape);
        h.frame(4);
        CHECK_FALSE(h.windowOpen("Define Attribute"));
    }
}

TEST_CASE("heavy undo and redo show a hint for a frame, then run") {
    Harness h(1920, 1080);
    h.core().setSetting("undo.busyBytes", "1");  // every step counts as heavy
    h.command("CIRCLE");
    h.core().submitText("0,0");
    h.core().submitText("10");
    h.frame(3);
    REQUIRE(entityCount(h) == 1);
    auto hintShown = [] {
        const ImGuiWindow* w = ImGui::FindWindowByName("##history");
        return w && w->Active;
    };
    auto press = [&](ImGuiKey k) {  // Ctrl+key, stopping after the frame that reads the key
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        h.frame();
        io.AddKeyEvent(k, true);
        h.frame();
        io.AddKeyEvent(k, false);
    };

    press(ImGuiKey_Z);
    CHECK(hintShown());  // hint drawn, step not run yet
    CHECK(entityCount(h) == 1);
    h.frame(2);
    CHECK_FALSE(hintShown());
    CHECK(entityCount(h) == 0);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    h.frame(2);

    press(ImGuiKey_Y);
    h.frame(3);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    h.frame(2);
    CHECK(entityCount(h) == 1);

    // Small steps (default limit) run at once, no hint.
    h.core().setSetting("undo.busyBytes", "1073741824");
    press(ImGuiKey_Z);
    CHECK_FALSE(hintShown());
    CHECK(entityCount(h) == 0);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    h.frame(2);
}

TEST_CASE("keyboard: layer dialogs close on Esc and take typing on open") {
    for (const char* opener : {"Layer states...", "Translate..."}) {
        const std::string win =
            std::string(opener) == "Layer states..." ? "Layer States Manager" : "Layer Translator";
        Harness h(1920, 1080);
        threeLayers(h);
        REQUIRE(layersMore(h, opener));
        h.frame(3);
        REQUIRE(h.windowOpen(win));
        h.key(ImGuiKey_Escape);
        h.frame(3);
        CHECK_FALSE(h.windowOpen(win));
    }
}

TEST_CASE("dimension styles panel: linear factor, fit and dragged text fields") {
    Harness h(1920, 1500);
    h.core().openPanel("dimstyles");
    h.frame(3);
    h.click("Dimension Styles", "DockSpace");
    h.frame(3);
    REQUIRE(h.click("Standard  (current)", "Dimension Styles"));
    h.frame(2);
    auto style = [&]() -> const DimStyle& {
        return *h.core().document().dimStyleByName("Standard");
    };
    REQUIRE(h.click("Primary Units", "Dimension Styles"));
    h.frame(3);
    REQUIRE(h.enterText("##Scale factor", "2", "Dimension Styles"));
    h.frame(3);
    CHECK(style().linearFactor == doctest::Approx(2.0));

    REQUIRE(h.click("Fit", "Dimension Styles"));
    h.frame(3);
    REQUIRE(h.click("##If not enough room", "Dimension Styles"));
    h.frame(2);
    REQUIRE(h.click("Text", "Combo"));
    h.frame(3);
    CHECK(style().fit == DimFit::TextFirst);

    REQUIRE(h.click("##Text not in default place", "Dimension Styles"));
    h.frame(2);
    REQUIRE(h.click("Over the line, without leader", "Combo"));
    h.frame(3);
    CHECK(style().textMove == DimTextMove::Free);
}

TEST_CASE("status bar: its controls sit on one line and are not cut off") {
    Harness h(1920, 1080);
    h.frame(4);
    const auto* snap = h.find("SNAP", "statusbar");
    const auto* layer = h.find("##layersw", "statusbar");
    const auto* scale = h.find("##annoscale", "statusbar");
    REQUIRE(snap);
    REQUIRE(layer);
    REQUIRE(scale);
    // Toggles and combo boxes share the row, and the boxes end inside the window.
    CHECK(layer->min.y >= snap->min.y - 6);
    CHECK(layer->max.y <= 1080.0f);
    CHECK(scale->max.y <= 1080.0f);
    CHECK(layer->min.y == doctest::Approx(scale->min.y));
}

TEST_CASE("save workspace dialog takes the keyboard when it opens and closes on Esc") {
    Harness h(1920, 1080);
    h.frame(3);
    REQUIRE(h.click("View"));
    h.frame(2);
    REQUIRE(h.click("Workspaces"));
    h.frame(2);
    REQUIRE(h.click("Save Current Layout As..."));
    h.frame(4);
    REQUIRE(h.windowOpen("Save workspace"));
    CHECK(ImGui::GetCurrentContext()->ActiveId != 0);  // the name field is ready to type in
    h.key(ImGuiKey_Escape);
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Save workspace"));
}

// A person who clicks, drags and types without a plan: whatever happens, nothing may crash, ImGui
// may not report misuse (the harness fails the test on that), and Esc must always bring the
// program back to an idle prompt.
TEST_CASE("random clicks, keys and commands leave the program usable") {
    Harness h(1440, 900);
    h.frame(3);
    for (const char* line : {"LINE 0,0 100,0 100,100 0,100 c", "CIRCLE 50,50 30",
                             "LINE 0,0 100,100", "RECTANGLE 10,10 40,40", "TEXT 20,20 5 0 Hello"}) {
        h.core().submitText(line);
        h.core().pressEscape();
    }
    h.core().zoomExtents();
    h.frame(3);
    const ImGuiKey keys[] = {ImGuiKey_Escape,    ImGuiKey_Enter,     ImGuiKey_Delete, ImGuiKey_Tab,
                             ImGuiKey_UpArrow,   ImGuiKey_DownArrow, ImGuiKey_F3,     ImGuiKey_F7,
                             ImGuiKey_F8,        ImGuiKey_F9,        ImGuiKey_F10,    ImGuiKey_F12,
                             ImGuiKey_Backspace, ImGuiKey_Space};
    const char* typed[] = {"LINE",   "CIRCLE", "MOVE",    "COPY",  "ERASE", "ZOOM",
                           "E",      "TRIM",   "OFFSET",  "HATCH", "10",    "5,5",
                           "@10<45", "u",      "close",   "all",   "LAYER", "PROPERTIES",
                           "ARRAY",  "MIRROR", "STRETCH", "x y"};
    // (TBCAD_FUZZ_SEED picks another sequence, TBCAD_FUZZ_STEPS a longer one)
    std::uint32_t state =
        std::getenv("TBCAD_FUZZ_SEED")
            ? static_cast<std::uint32_t>(std::atoll(std::getenv("TBCAD_FUZZ_SEED")))
            : 20261019u;
    const bool menus = std::getenv("TBCAD_FUZZ_MENUS") != nullptr;
    const int steps =
        std::getenv("TBCAD_FUZZ_STEPS") ? std::atoi(std::getenv("TBCAD_FUZZ_STEPS")) : 250;
    auto rnd = [&](std::uint32_t n) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % n;
    };
    for (int step = 0; step < steps; ++step) {
        const float x = 40.0f + static_cast<float>(rnd(1360));
        const float y =
            (menus ? 2.0f : 70.0f) +
            static_cast<float>(rnd(menus ? 850 : 780));  // (not the menu bar: file dialogs)
        switch (rnd(11)) {
            case 8:
                h.core().newDocument();
                break;
            case 9:
                h.core().closeDocument(rnd(static_cast<std::uint32_t>(h.core().documentCount())));
                break;
            case 10:
                h.core().activateDocument(
                    rnd(static_cast<std::uint32_t>(h.core().documentCount())));
                h.core().submitText(rnd(2) ? "LAYOUT New" : "LAYOUT Move");
                break;
            case 0:
                h.click(x, y);
                break;
            case 1:
                h.click(x, y, 1);
                break;
            case 2:
                h.drag(x, y, 40.0f + static_cast<float>(rnd(1360)),
                       70.0f + static_cast<float>(rnd(780)));
                break;
            case 3:
                h.scroll(x, y, static_cast<float>(rnd(5)) - 2.0f);
                break;
            case 4:
                h.key(keys[rnd(sizeof keys / sizeof *keys)], rnd(4) == 0, rnd(5) == 0);
                break;
            case 5:
                h.core().submitText(typed[rnd(sizeof typed / sizeof *typed)]);
                break;
            case 6:
                h.doubleClick(x, y);
                break;
            default:
                h.moveMouse(x, y);
                break;
        }
        h.frame(1);
    }
    // Back to idle: Esc ends commands, popups and dialogs.
    for (int i = 0; i < 6; ++i) {
        h.key(ImGuiKey_Escape);
        h.core().pressEscape();
        h.frame(2);
    }
    CHECK_FALSE(h.core().runner().active());
    h.core().submitText("LINE 0,0 5,5");
    h.core().pressEscape();
    h.frame(2);
    CHECK(h.core().document().modelSpaceCount() > 0);
}

namespace {

struct FullDrawing {
    EntityId line, text, mtext, dim, hatch, insert, circle;
};

// A drawing with something in every table a panel lists: layers in each state, text and dimension
// styles, a block with an attribute and two references, text, MText, a dimension, a hatch, a
// layout and an attached external reference. Saved and opened again so the scene is built the
// way it is for a real file.
FullDrawing fullDrawing(Harness& h) {
    const fs::path dir = scratchDir("full");
    h.core().submitText("CIRCLE");
    h.core().submitText("0,0");
    h.core().submitText("5");
    h.core().pressEscape();
    const fs::path refFile = dir / "ref.tbcad";
    REQUIRE(h.core().saveAs(refFile).ok());
    h.core().newDocument();
    h.frame(2);

    FullDrawing out{};
    {
        Document& doc = h.core().document();
        Transaction tx = doc.begin("setup");
        LayerId walls{}, frozen{};
        {
            Layer l;
            l.name = "T-Walls";
            walls = tx.addLayer(l).value();
            l.name = "T-Frozen";
            l.frozen = true;
            frozen = tx.addLayer(l).value();
            l.name = "T-Locked";
            l.frozen = false;
            l.locked = true;
            REQUIRE(tx.addLayer(l).ok());
            l.name = "T-Off";
            l.locked = false;
            l.on = false;
            REQUIRE(tx.addLayer(l).ok());
        }
        TextStyle ts;
        ts.name = "T-Notes";
        const TextStyleId notes = tx.addTextStyle(ts).value();
        DimStyle ds = *doc.dimStyle(doc.header().currentDimStyle);
        ds.id = {};
        ds.name = "T-Arch";
        const DimStyleId arch = tx.addDimStyle(ds).value();

        const BlockId door = tx.addBlock(BlockDef{.name = "T-Door"}).value();
        (void)tx.add(Entity{{}, LineData{{0, 0}, {9, 0}}}, door);
        TextData tag;
        tag.utf8 = "D1";
        tag.height = 2;
        tag.attdef =
            Boxed<AttDefInfo>(AttDefInfo{.tag = "MARK", .prompt = "Mark", .defaultValue = "D1"});
        (void)tx.add(Entity{{}, Indirect<TextData>(tag)}, door);

        auto add = [&](EntityData d, LayerId layer = {}) {
            Entity e{{}, std::move(d)};
            e.common.layer = layer;
            return tx.add(std::move(e)).value();
        };
        out.line = add(LineData{{0, 0}, {100, 0}}, walls);
        (void)add(LineData{{0, 10}, {100, 10}}, frozen);
        out.circle = add(CircleData{{50, 50}, 10});
        TextData t;
        t.utf8 = "Room 101";
        t.height = 2.5;
        t.style = notes;
        t.position = {10, 20};
        out.text = add(Indirect<TextData>(t));
        MTextData m;
        m.utf8 = "{\\H2x;Big} note";
        m.height = 2.5;
        m.position = {10, 30};
        out.mtext = add(Indirect<MTextData>(m));
        DimensionData d;
        d.style = arch;
        d.defPts = {Vec2{0, 0}, Vec2{100, 0}, Vec2{50, -10}, Vec2{0, 0}};
        d.textPos = {50, -10};
        out.dim = add(Indirect<DimensionData>(d));
        HatchData hd;
        hd.solid = false;
        hd.pattern = "ANSI31";
        HatchLoop loop;
        loop.edges.push_back(HatchPolylineEdge{{PolyVertex{{60, 60}}, PolyVertex{{80, 60}},
                                                PolyVertex{{80, 80}}, PolyVertex{{60, 80}}},
                                               true});
        hd.loops.push_back(loop);
        out.hatch = add(Indirect<HatchData>(hd));
        InsertData in;
        in.block = door;
        in.position = {120, 0};
        in.attribs.push_back(AttribValue{"MARK", "D7"});
        out.insert = add(Indirect<InsertData>(in));
        in.position = {140, 0};
        (void)add(Indirect<InsertData>(in));
        tx.commit();
    }
    h.core().submitText("LAYOUT");
    h.core().submitText("New");
    h.core().submitText("Sheet-A");
    h.core().pressEscape();
    h.core().submitText("XATTACH");
    h.core().submitText(refFile.string());
    h.core().submitText("Attach");
    h.core().submitText("Absolute");
    h.core().submitText("ref");
    h.core().submitText("200,0");
    h.core().pressEscape();
    const fs::path file = dir / "full.tbcad";
    REQUIRE(h.core().saveAs(file).ok());
    h.core().newDocument();
    h.frame(2);
    REQUIRE(h.core().openFile(file).ok());
    h.frame(3);
    REQUIRE(h.core().document().find(out.insert) != nullptr);
    return out;
}

// Clicks each widget of the panel's window (twice over, so a row picked in the first pass is
// edited in the second), types a value into any text field a click focused, and closes whatever
// it opened.
void clickEveryWidget(Harness& h, const char* tab) {
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::string> labels;
        for (const auto& it : imgui_items::items())
            if (it.window.find(tab) != std::string::npos && !it.label.empty() && it.label != tab)
                labels.push_back(it.label);
        for (const std::string& l : labels) {
            if (!h.has(l, tab))
                continue;
            h.click(l, tab);
            h.frame(2);
            if (ImGui::GetIO().WantTextInput) {
                h.key(ImGuiKey_A, /*ctrl=*/true);
                h.type("3");
                h.key(ImGuiKey_Enter);
                h.frame(2);
            }
            h.key(ImGuiKey_Escape);
            h.frame(1);
            h.core().pressEscape();  // a command a click started
            h.frame(1);
            if (!h.windowOpen(tab) && h.has(tab, "DockSpace"))
                h.click(tab, "DockSpace");
        }
    }
}

}  // namespace

TEST_CASE("panels on a full drawing: every widget clicked, with each kind of object selected") {
    struct P {
        const char* id;
        const char* tab;
    };
    const P panels[] = {{"layers", "Layers"},
                        {"properties", "Object Properties"},
                        {"levels", "Levels"},
                        {"blocks", "Blocks"},
                        {"xrefs", "External References"},
                        {"textstyles", "Text Styles"},
                        {"dimstyles", "Dimension Styles"},
                        {"hatch", "Hatch"},
                        {"palettes", "Tool Palettes"},
                        {"plotstyle", "Plot Styles"},
                        {"laywalk", "Layer Walk"},
                        {"attvalues", "Attribute Values"},
                        {"content", "Content Browser"},
                        {"mleaderstyles", "Multileader Style Manager"},
                        {"tablestyles", "Table Style"},
                        {"mlstyles", "Multiline Style"},
                        {"views", "View Manager"},
                        {"ucsman", "UCS"},
                        {"vports", "Viewports"},
                        {"pointstyle", "Point Style"}};
    for (const P& p : panels) {
        const std::string panelId = p.id;
        CAPTURE(panelId);
        Harness h(1920, 1080);
        const FullDrawing d = fullDrawing(h);
        // The properties panel draws different sections per kind: try each; the others once
        // with everything selected.
        std::vector<std::vector<EntityId>> selections = {{}};
        if (std::string_view(p.id) == "properties" || std::string_view(p.id) == "attvalues")
            selections = {{d.line}, {d.circle}, {d.text},   {d.mtext},
                          {d.dim},  {d.hatch},  {d.insert}, {d.line, d.text, d.insert}};
        for (const auto& sel : selections) {
            if (sel.empty())
                h.core().selectAll();
            else
                h.core().setSelection(sel);
            h.core().openPanel(p.id);
            h.frame(3);
            REQUIRE(h.has(p.tab, "DockSpace"));
            REQUIRE(h.click(p.tab, "DockSpace"));
            h.frame(3);
            clickEveryWidget(h, p.tab);
            // Still a working drawing: undo everything the clicks did, then draw a line.
            for (int i = 0; i < 50 && h.core().undo(); ++i) {
            }
            h.frame(2);
        }
        h.core().submitText("LINE");
        h.core().submitText("0,0");
        h.core().submitText("1,1");
        h.core().pressEscape();
        h.frame(2);
    }
}

TEST_CASE("block editor: BEDIT opens Block Authoring, its buttons run the editor's commands") {
    Harness h(1920, 1080);
    h.frame(3);
    for (const char* s : {"LINE", "0,0", "10,0", ""})
        h.core().submitText(s);
    h.core().selectAll();
    for (const char* s : {"-BLOCK", "Sym", "0,0", "C"})
        h.core().submitText(s);
    h.frame(3);
    REQUIRE(h.core().document().blockByName("Sym"));
    for (const char* s : {"BEDIT", "Name", "Sym"})
        h.core().submitText(s);
    h.frame(6);
    REQUIRE(h.windowOpen("Block Authoring"));
    CHECK(h.has("Save As...", "Block Authoring"));
    REQUIRE(h.click("New state...", "Block Authoring"));
    h.frame(3);
    for (const char* s : {"New", "Open", "Show"})
        h.core().submitText(s);
    h.frame(4);
    CHECK(h.has("Open", "Block Authoring"));
    REQUIRE(h.click("Close", "Block Authoring"));
    h.frame(3);
    h.core().submitText("");  // Save
    h.frame(4);
    CHECK_FALSE(h.windowOpen("Block Authoring"));
    const BlockDef* def = h.core().document().blockByName("Sym");
    REQUIRE((def && def->dynamic));
    CHECK(def->dynamic->params[0].states.size() == 2);
}

TEST_CASE(
    "blocks palette: double-click inserts with the palette's options, drag lands at the drop") {
    Harness h(1920, 1080);
    h.frame(3);
    for (const char* s : {"LINE", "0,0", "10,0", ""})
        h.core().submitText(s);
    h.core().selectAll();
    for (const char* s : {"-BLOCK", "Sym", "0,0", "R"})
        h.core().submitText(s);
    h.core().submitText("ZOOM");
    h.core().submitText("W");
    h.core().submitText("-100,-100");
    h.core().submitText("100,100");
    h.core().setSetting("block.palette.scale", "2");
    h.core().submitText("BLOCKSPALETTE");
    h.frame(6);
    REQUIRE(h.windowOpen("Blocks"));
    CHECK(h.has("Current Drawing", "Blocks"));
    CHECK(h.has("Recent", "Blocks"));
    CHECK(h.has("Other Drawing", "Blocks"));
    const auto* tile = h.find("##tile", "Blocks");
    REQUIRE(tile);
    const ImVec2 at((tile->min.x + tile->max.x) / 2, (tile->min.y + tile->max.y) / 2);
    h.doubleClick(at.x, at.y);
    h.frame(3);
    h.core().submitText("30,40");
    h.frame(3);
    auto inserts = [&] {
        std::size_t n = 0;
        h.core().document().forEachEntity(
            [&](const Entity& e) { n += kind(e) == EntityKind::Insert; });
        return n;
    };
    REQUIRE(inserts() == 1);
    const InsertData* first = nullptr;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            first = i;
    });
    REQUIRE(first);
    CHECK(first->position == Vec2{30, 40});
    CHECK(first->scale == Vec2{2, 2});

    // Drag the tile onto the drawing: placed where it is dropped.
    const ImVec2 drop = screenOf(h, {-50, -20});
    h.drag(at.x, at.y, drop.x, drop.y);
    h.frame(4);
    REQUIRE(inserts() == 2);
    bool found = false;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* i = entityAs<InsertData>(e))
            found |= distance(i->position, {-50, -20}) < 2.0;
    });
    CHECK(found);
    CHECK_FALSE(h.core().runner().active());
    // Both show up in Recent.
    CHECK(block_cmds::parseRecent(h.core().setting("block.recent").value_or("")).size() == 1);
}

TEST_CASE("contextual tab: tools for a text selection, gone for a mixed one") {
    Harness h(1920, 1080);
    h.frame(3);
    for (const char* s : {"LINE", "0,10", "10,10", ""})
        h.core().submitText(s);
    for (const char* s : {"TEXT", "0,0", "2.5", "0", "Hello", ""})
        h.core().submitText(s);
    h.core().pressEscape();  // however many prompts TEXT still had
    h.frame(3);
    REQUIRE_FALSE(h.core().runner().active());
    const std::vector<EntityId> all = h.core().document().modelSpace();
    std::vector<EntityId> text;
    for (EntityId id : all)
        if (kind(*h.core().document().find(id)) == EntityKind::Text)
            text.push_back(id);
    REQUIRE(text.size() == 1);
    h.core().setSelection(text);
    h.frame(3);
    CHECK(h.windowOpen("contexttab"));
    CHECK(h.has("Spelling", "contexttab"));
    h.core().setSelection(all);
    h.frame(3);
    CHECK_FALSE(h.windowOpen("contexttab"));
    // Close clears the selection; ui.contextTabs 0 turns the strip off.
    h.core().setSelection(text);
    h.frame(3);
    REQUIRE(h.click("Close", "contexttab"));
    h.frame(3);
    CHECK(h.core().selection().empty());
    h.core().setSetting("ui.contextTabs", "0");
    h.core().setSelection(text);
    h.frame(3);
    CHECK_FALSE(h.windowOpen("contexttab"));
}

TEST_CASE("option bar: options of the running command are buttons, and can be hidden") {
    Harness h(1920, 1080);
    h.frame(3);
    CHECK_FALSE(h.windowOpen("optionbar"));
    h.core().submitText("PLINE");
    h.core().submitText("0,0");
    h.core().submitText("10,0");
    h.frame(3);
    REQUIRE(h.windowOpen("optionbar"));
    REQUIRE(h.has("Undo", "optionbar"));
    const auto lines = h.core().history().size();
    REQUIRE(h.click("Undo", "optionbar"));
    h.frame(3);
    CHECK(h.core().history().size() > lines);
    // Selection prompts offer the selection modes.
    h.core().pressEscape();
    h.core().pressEscape();
    h.core().submitText("ERASE");
    h.frame(3);
    CHECK(h.has("Crossing", "optionbar"));
    h.core().pressEscape();
    h.core().setSetting("ui.optionBar", "0");
    h.core().submitText("PLINE");
    h.core().submitText("0,0");
    h.core().submitText("10,0");
    h.frame(3);
    CHECK_FALSE(h.windowOpen("optionbar"));
    h.core().pressEscape();
}

TEST_CASE("status bar: VIS and ISO buttons, items hidden from the customization menu") {
    Harness h(1920, 1080);
    h.frame(3);
    REQUIRE(h.has("VIS", "statusbar"));
    REQUIRE(h.has("ISO", "statusbar"));
    const bool before = annoAllVisible(h.core().document().header());
    REQUIRE(h.click("VIS", "statusbar"));
    h.frame(3);
    CHECK(annoAllVisible(h.core().document().header()) != before);
    // Hide two items with the menu at the end of the bar.
    REQUIRE(h.click("...", "statusbar"));
    h.frame(2);
    REQUIRE(h.click("Isolate objects (ISO)"));
    h.frame(3);
    CHECK_FALSE(h.has("ISO", "statusbar"));
    CHECK(h.core().setting("statusbar.hidden").value_or("") == "isolate");
    REQUIRE(h.click("...", "statusbar"));
    h.frame(2);
    REQUIRE(h.click("Show all"));
    h.frame(3);
    CHECK(h.has("ISO", "statusbar"));
}

TEST_CASE("Window commands: COMMANDLINEHIDE, PROPERTIESCLOSE, HIDEPALETTES and SHOWPALETTES") {
    Harness h(1920, 1080);
    h.frame(3);
    REQUIRE(h.windowOpen("Command"));
    h.core().submitText("COMMANDLINEHIDE");
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Command"));
    h.core().submitText("COMMANDLINE");
    h.frame(3);
    CHECK(h.windowOpen("Command"));
    h.core().submitText("HIDEPALETTES");
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Layers"));
    h.core().submitText("SHOWPALETTES");
    h.frame(3);
    CHECK(h.windowOpen("Layers"));
    h.core().submitText("PROPERTIESCLOSE");
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Properties"));
    h.core().submitText("GRAPHSCR");
    h.frame(3);
}

TEST_CASE("Clean Screen from the command line and Ctrl+0, workspaces with WSSAVE and WSCURRENT") {
    Harness h(1920, 1080);
    h.frame(3);
    REQUIRE(h.windowOpen("statusbar"));
    h.core().submitText("CLEANSCREENON");
    h.frame(3);
    CHECK_FALSE(h.windowOpen("statusbar"));
    h.core().submitText("CLEANSCREENOFF");
    h.frame(3);
    CHECK(h.windowOpen("statusbar"));
    h.key(ImGuiKey_0, true);
    h.frame(3);
    CHECK_FALSE(h.windowOpen("statusbar"));
    h.key(ImGuiKey_0, true);
    h.frame(3);
    CHECK(h.windowOpen("statusbar"));

    h.core().submitText("WSSAVE");
    h.core().submitText("Mine");
    h.frame(3);
    CHECK(h.core().setting("workspace.current").value_or("") == "Mine");
    h.core().submitText("WSCURRENT");
    h.core().submitText("Drafting");
    h.frame(3);
    CHECK(h.core().setting("workspace.current").value_or("") == "Drafting");
    h.core().submitText("WSSAVE");
    h.core().submitText("Drafting");  // built in
    h.frame(2);
    CHECK(h.core().setting("workspace.current").value_or("") == "Drafting");
}

TEST_CASE("Ctrl+1 toggles Object Properties and Ctrl+9 the command line, as in AutoCAD") {
    Harness h(1920, 1080);
    h.frame(3);
    REQUIRE(h.windowOpen("Object Properties"));
    h.key(ImGuiKey_1, true);
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Object Properties"));
    h.key(ImGuiKey_1, true);
    h.frame(3);
    CHECK(h.windowOpen("Object Properties"));
    h.key(ImGuiKey_9, true);
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Command"));
    h.key(ImGuiKey_9, true);
    h.frame(3);
    CHECK(h.windowOpen("Command"));
}

TEST_CASE("Ctrl+Q exits and Ctrl+B toggles snap, as in AutoCAD") {
    Harness h(1920, 1080);
    h.frame(3);
    const auto snap = [&] { return h.core().setting("drafting.snap").value_or("0") == "1"; };
    CHECK_FALSE(snap());
    h.key(ImGuiKey_B, true);
    h.frame(3);
    CHECK(snap());
    CHECK_FALSE(h.gui().wantsExit());
    h.key(ImGuiKey_Q, true);
    h.frame(3);
    CHECK(h.gui().wantsExit());
}

TEST_CASE("Publish dialog: the drawing's sheets, save the list, publish one PDF") {
    const fs::path dir = scratchDir("publish");
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,50");
    h.core().submitText("");
    REQUIRE(h.core().saveAs(dir / "pub.tbcad").ok());
    h.core().submitText("PUBLISH");
    h.frame(3);
    REQUIRE(h.windowOpen("Publish"));
    REQUIRE(h.click("Save list", "Publish"));
    h.frame(2);
    CHECK(slurp(dir / "pub.tbdsd").find("pub.tbcad") != std::string::npos);
    REQUIRE(h.click("Publish to PDF", "Publish"));
    h.frame(3);
    CHECK(fs::exists(dir / "pub.pdf"));
}

TEST_CASE("Plot Frames panel: check sheets and plot them into one PDF") {
    const fs::path dir = scratchDir("plotframes");
    Harness h(1920, 1080);
    for (const char* t : {"LINE", "0,0", "90000,20000", ""})
        h.core().submitText(t);
    REQUIRE(h.core().saveAs(dir / "frames.tbcad").ok());
    // Plots go to the folder used last (never the drawing's own).
    fs::create_directories(dir / "out");
    h.core().setSetting("plot.file.pdf", (dir / "out" / "last.pdf").string());
    for (const char* corner : {"0,0", "50000,0"}) {
        h.core().submitText("PLOTFRAME");
        for (const char* t : {"New", "A3", "Landscape", "1:100", corner, ""})
            h.core().submitText(t);
    }
    h.frame(2);
    h.core().submitText("PLOTFRAMEPANEL");
    h.frame(3);
    REQUIRE(h.windowOpen("Plot Frames"));
    REQUIRE(h.click("All", "Plot Frames"));
    h.frame(2);
    REQUIRE(h.click("Plot checked", "Plot Frames"));
    h.frame(3);
    CHECK(fs::exists(dir / "out" / "frames-plot.pdf"));
    bool plotted = false;
    for (const auto& line : h.core().history())
        plotted = plotted || line.find("Plotted 2 plot frames") != std::string::npos;
    CHECK(plotted);
}

TEST_CASE("Purge dialog removes an unused layer, the units, properties and recovery windows open") {
    Harness h(1920, 1080);
    run(h, "-LAYER New Junk");
    auto hasJunk = [&] {
        for (const auto& [id, row] : h.core().document().layers())
            if (row.name == "Junk")
                return true;
        return false;
    };
    REQUIRE(hasJunk());
    h.core().submitText("PURGE");
    h.frame(3);
    REQUIRE(h.windowOpen("Purge"));
    REQUIRE(h.click("Purge Checked Items", "Purge"));
    h.frame(3);
    CHECK_FALSE(hasJunk());

    h.core().submitText("UNITS");
    h.frame(3);
    CHECK(h.windowOpen("Drawing Units"));
    h.core().submitText("DWGPROPS");
    h.frame(3);
    CHECK(h.windowOpen("Drawing Properties"));
    h.core().submitText("DRAWINGRECOVERY");
    h.frame(3);
    CHECK(h.windowOpen("Drawing Recovery"));
}

TEST_CASE("Quick Select and Object Selection Filters dialogs select from the drawing") {
    Harness h(1920, 1080);
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,0");
    h.core().submitText("");
    h.core().submitText("CIRCLE");
    h.core().submitText("50,50");
    h.core().submitText("10");
    h.core().clearSelection();
    h.core().submitText("QSELECT");
    h.frame(3);
    REQUIRE(h.windowOpen("Quick Select"));
    // Layer = "" with Exclude: every object, since none is on a layer with no name.
    REQUIRE(h.click("Exclude from new selection set", "Quick Select"));
    REQUIRE(h.click("Select Matching", "Quick Select"));
    h.frame(2);
    CHECK(h.core().selection().size() == 2);

    h.core().submitText("FILTER");
    h.frame(3);
    CHECK(h.windowOpen("Object Selection Filters"));
    h.core().clearSelection();
    h.core().submitText("SELECTSIMILAR");
    h.core().submitText("Settings");
    h.frame(3);
    CHECK(h.windowOpen("Select Similar Settings"));
    h.core().pressEscape();
}

TEST_CASE("double click edits each type with its own command, as AutoCAD's CUI sets it") {
    Harness h(1920, 1080);
    h.frame(3);
    ui::AppCore& c = h.core();
    for (const char* s : {"PLINE", "0,0", "100,0", "100,100", "0,100", "C"})
        c.submitText(s);
    for (const char* s : {"LINE", "300,0", "320,0", ""})
        c.submitText(s);
    {
        // A block "Mark" of one line, inserted at (300, 50).
        Transaction tx = c.document().begin("block");
        BlockDef def;
        def.name = "Mark";
        const BlockId b = tx.addBlock(std::move(def)).value();
        (void)tx.add(Entity{{}, LineData{{0, 0}, {20, 0}}}, b);
        InsertData ins;
        ins.block = b;
        ins.position = {300, 50};
        (void)tx.add(Entity{{}, std::move(ins)});
        tx.commit();
    }
    c.pressEscape();
    c.zoomExtents();
    h.frame(3);
    auto dbl = [&](Vec2 world) {
        c.pressEscape();
        c.clearSelection();
        h.frame(2);
        const ImVec2 at = screenOf(h, world);
        h.doubleClick(at.x, at.y);
        h.frame(3);
    };
    // A polyline goes straight to PEDIT's options: the double-clicked one is taken.
    dbl({50, 0});
    CHECK(runningName(h) == "PEDIT");
    REQUIRE(c.runner().prompt());
    CHECK(c.runner().prompt()->text.rfind("Select", 0) != 0);
    c.pressEscape();
    // A block opens the block editor on that block.
    REQUIRE(c.document().blockByName("Mark"));
    dbl({310, 50});
    CHECK(c.setting("tmp.bedit.block").value_or("") == "Mark");
    c.submitText("BCLOSE");
    c.submitText("Discard");
    h.frame(3);
    // A different action set on the Customize page.
    c.settings().setString("dblclick.Polyline", "LIST");
    dbl({50, 0});
    bool listed = false;
    for (const auto& line : c.history())
        listed = listed || line.find("POLYLINE") != std::string::npos ||
                 line.find("Polyline") != std::string::npos;
    CHECK(listed);
}

TEST_CASE("Customize page: Quick Access buttons and own right-click menu commands") {
    Harness h(1920, 1080);
    h.frame(3);
    h.core().submitText("CUI");
    h.frame(4);
    REQUIRE(h.windowOpen("Settings"));
    CHECK(h.has("Double-click edits the object (DBLCLKEDIT)", "Settings"));
    CHECK(h.has("##dblclick.Polyline", "Settings"));
    // A command typed in the first list becomes a button on the toolbar.
    REQUIRE(h.enterText("##add", "circle", "Settings", 0));
    h.frame(3);
    CHECK(h.core().settings().getStrings("ui.quickAccess") == std::vector<std::string>{"circle"});
    // An unknown command is refused.
    REQUIRE(h.enterText("##add", "nosuchthing", "Settings", 1));
    h.frame(3);
    CHECK(h.core().settings().getStrings("ui.contextMenu").empty());
    h.key(ImGuiKey_Escape);
    h.frame(3);
    REQUIRE(h.click("CIRCLE##quick"));
    h.frame(2);
    CHECK(runningName(h) == "CIRCLE");
    h.core().pressEscape();

    // Own commands at the end of the drawing's right-click menu.
    h.core().settings().setStrings("ui.contextMenu", {"ZOOM E"});
    // An empty drawing shows the start page over the view; a drawn line takes it away.
    h.core().submitText("LINE");
    h.core().submitText("100,100");
    h.core().submitText("400,100");
    h.core().pressEscape();
    h.frame(3);
    const ImVec2 at = screenOf(h, h.core().camera().center);
    h.click(at.x, at.y, 1);
    h.frame(3);
    REQUIRE(h.click("ZOOM E", "Popup"));
    h.frame(3);
    bool zoomed = false;
    for (const auto& line : h.core().history())
        zoomed = zoomed || line.find("ZOOM") != std::string::npos;
    CHECK(zoomed);
}

TEST_CASE(
    "open drawings: SAVEALL, QVDRAWING, close other drawings and CLOSEALL with the save question") {
    Harness h(1600, 900);
    const fs::path dir = scratchDir("drawings");
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("100,0");
    h.core().pressEscape();
    REQUIRE(h.core().saveAs(dir / "one.tbcad").ok());
    h.core().submitText("CIRCLE");
    h.core().submitText("0,0");
    h.core().submitText("5");
    h.frame(2);
    h.core().newDocument();
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("0,100");
    h.core().pressEscape();
    REQUIRE(h.core().saveAs(dir / "two.tbcad").ok());
    h.core().newDocument();
    h.frame(2);
    REQUIRE(h.core().documentCount() == 3);
    CHECK(h.core().documentModified(0));

    h.core().submitText("SAVEALL");
    h.frame(3);
    CHECK_FALSE(h.core().anyDocumentModified());

    // Quick view: a card per drawing, and a click on a layout button switches to it.
    h.core().submitText("QVD");
    h.frame(3);
    REQUIRE(h.windowOpen("Quick View: Drawings"));
    REQUIRE(h.click("Layout1", "Quick View"));  // the first drawing's
    h.frame(3);
    CHECK(h.core().activeDocument() == 0);
    CHECK(h.core().activeLayout() == "Layout1");
    CHECK_FALSE(h.windowOpen("Quick View"));
    h.core().setActiveLayout("");
    h.frame(2);

    // EXPORT picks the format by the file name.
    h.core().submitText("FILEDIA");
    h.core().submitText("0");
    h.core().submitText("EXPORT");
    h.core().submitText((dir / "one-out.dxf").string());
    h.frame(4);
    CHECK(fs::exists(dir / "one-out.dxf"));

    // Close the other drawings: only "one" is left.
    h.core().submitText("CIRCLE");
    h.core().submitText("50,50");
    h.core().submitText("5");
    h.frame(2);
    REQUIRE(h.core().activateDocument(1));
    h.core().submitText("CIRCLE");  // a change in a drawing that will be closed
    h.core().submitText("50,50");
    h.core().submitText("5");
    h.frame(2);
    REQUIRE(h.core().activateDocument(0));
    h.frame(2);
    REQUIRE(h.rightClick("one.tbcad", "##doctabs"));
    h.frame(2);
    REQUIRE(h.click("Close Other Drawings"));
    h.frame(3);
    REQUIRE(h.has("Don't Save"));  // asked about "two"
    REQUIRE(h.click("Don't Save"));
    h.frame(3);
    CHECK(h.core().documentCount() == 1);
    CHECK(h.core().file().filename() == "one.tbcad");
    CHECK(h.core().modified());

    h.core().submitText("CLOSEALL");
    h.frame(3);
    REQUIRE(h.click("Don't Save"));
    h.frame(3);
    CHECK(h.core().documentCount() == 1);
    CHECK(h.core().isPristine());
}

TEST_CASE("CLOSEALL with several unsaved drawings asks one combined question") {
    Harness h(1600, 900);
    const auto edit = [&] {
        h.core().submitText("CIRCLE");
        h.core().submitText("0,0");
        h.core().submitText("5");
        h.frame(2);
    };
    edit();
    h.core().newDocument();
    edit();
    h.core().newDocument();
    edit();
    h.frame(2);
    REQUIRE(h.core().documentCount() == 3);
    h.core().submitText("CLOSEALL");
    h.frame(3);
    REQUIRE(h.has("Discard all"));
    REQUIRE(h.click("Cancel"));
    h.frame(3);
    CHECK(h.core().documentCount() == 3);
    h.core().submitText("CLOSEALL");
    h.frame(3);
    REQUIRE(h.click("Discard all"));
    h.frame(3);
    CHECK(h.core().documentCount() == 1);
    CHECK(h.core().isPristine());
}

#include "gl_problem.h"

TEST_CASE("OpenGL problem message names the GPU and the fix") {
    const std::string a = tbcad::openGlProblemText("GDI Generic", "1.1.0");
    CHECK(a.find("GDI Generic") != std::string::npos);
    CHECK(a.find("1.1.0") != std::string::npos);
    CHECK(a.find("basic display driver") != std::string::npos);
    const std::string b = tbcad::openGlProblemText("", "");
    CHECK(b.find("unknown") != std::string::npos);
    CHECK(b.find("Update the graphics driver") != std::string::npos);
    CHECK(b.find("--software-gl") != std::string::npos);
}

TEST_CASE("first run: the start page opens the sample or a new drawing in one click, no dialog") {
    {
        Harness h(1600, 900);  // an empty config folder, as on a first run
        bool welcomed = false;
        for (const auto& line : h.core().history())
            welcomed = welcomed || line.find("Welcome to TBCAD") != std::string::npos;
        CHECK(welcomed);
        CHECK(h.core().isPristine());
        REQUIRE(h.has("New drawing", "##start"));
        REQUIRE(h.has("Open sample drawing", "##start"));
        REQUIRE(h.click("Open sample drawing", "##start"));
        h.settle();
        CHECK_FALSE(h.has("New drawing", "##start"));
        CHECK_FALSE(h.core().isPristine());
        CHECK(h.core().documentCount() == 1);
        CHECK(h.core().hasFile());
        CHECK_FALSE(h.windowOpen("Save"));
        CHECK_FALSE(h.windowOpen("Open"));
        CHECK(h.imguiErrors().empty());
    }
    Harness h(1600, 900);
    REQUIRE(h.click("New drawing", "##start"));
    h.settle();
    CHECK_FALSE(h.has("New drawing", "##start"));
    CHECK(h.core().documentCount() >= 1);
    CHECK_FALSE(h.windowOpen("Open"));
    CHECK_FALSE(h.windowOpen("Save"));
}

TEST_CASE("point prompts: a shaky click does not add a second point on release") {
    // Hand jitter while the button is down made a stray short segment at the start of a LINE.
    CHECK_FALSE(tbcad::app::releaseTakesPoint(6, 0.08, 1.0));
    CHECK_FALSE(tbcad::app::releaseTakesPoint(30, 0.1, 1.0));
    CHECK_FALSE(tbcad::app::releaseTakesPoint(5, 1.0, 1.0));
    // A deliberate press-drag-release still answers the next point.
    CHECK(tbcad::app::releaseTakesPoint(200, 0.1, 1.0));
    CHECK(tbcad::app::releaseTakesPoint(20, 0.5, 1.0));
    CHECK_FALSE(tbcad::app::releaseTakesPoint(14, 0.5, 2.0));
}

TEST_CASE("PLINE: a click with a shaky hand takes one point and the next click adds the next") {
    Harness h(1920, 1080);
    h.core().setSetting("drafting.osnap", "0");
    h.frame(3);
    h.core().submitText("PLINE");
    h.frame(3);
    ImGuiIO& io = ImGui::GetIO();
    h.moveMouse(700, 500);
    h.frame();
    io.AddMouseButtonEvent(0, true);
    h.frame();
    h.moveMouse(706, 503);  // the hand moves a little while the button is down
    io.AddMouseButtonEvent(0, false);
    h.frame(2);
    // Still asking for the next point; the line has not been made yet.
    REQUIRE(h.core().runner().active());
    CHECK(entityCount(h) == 0);
    h.moveMouse(900, 520);
    h.frame(2);
    h.click(900, 520);
    h.core().submitText("");
    h.frame(3);
    std::size_t n = 0;
    h.core().document().forEachEntity([&](const Entity& e) {
        if (const auto* p = std::get_if<PolylineData>(&e.data))
            n = p->v.size();
    });
    CHECK(n == 2);
}

TEST_CASE("window link: windows find each other and pass drawings on") {
    const fs::path dir = fs::temp_directory_path() / "tbcad-window-link-test";
    fs::remove_all(dir);
    {
        app::WindowLink a(dir, 4001), b(dir, 4002);
        CHECK(a.others().empty());
        a.publish({10, 20, 300, 200}, true);
        b.publish({500, 0, 400, 300}, true);
        const auto seen = a.others();
        REQUIRE(seen.size() == 1);
        CHECK(seen[0].pid == 4002);
        CHECK(seen[0].rect.contains(600, 100));
        CHECK_FALSE(seen[0].rect.contains(100, 100));
        CHECK(a.send(4002, fs::path("/tmp/some drawing.dxf")));
        CHECK(a.takeIncoming().empty());  // not for a
        const auto got = b.takeIncoming();
        REQUIRE(got.size() == 1);
        CHECK(got[0] == "/tmp/some drawing.dxf");
        CHECK(b.takeIncoming().empty());  // each only once
    }
    CHECK(app::WindowLink(dir, 4003).others().empty());  // a window that closed is gone
    fs::remove_all(dir);
}

TEST_CASE("drawing tab dragged onto another window moves the drawing there") {
    const fs::path dir = fs::temp_directory_path() / "tbcad-tab-move-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Harness h(1600, 900);
    app::WindowLink me(dir / "link", 5001), other(dir / "link", 5002);
    other.publish({4000, 0, 800, 600}, true);  // far off the harness window
    h.gui().setWindowLink(&me);
    const fs::path file = dir / "move me.tbcad";
    REQUIRE(h.core().saveAs(file));
    h.frame(3);
    const std::string title = h.core().documentTitle(0);

    // A modified drawing stays: it would have to be saved first.
    h.core().submitText("LINE");
    h.core().submitText("0,0");
    h.core().submitText("10,0");
    h.core().pressEscape();
    h.frame(2);
    REQUIRE(h.core().documentModified(0));
    {
        const auto* tab = h.find(title);
        REQUIRE(tab);
        const imgui_items::Item t = *tab;
        h.drag((t.min.x + t.max.x) * 0.5f, (t.min.y + t.max.y) * 0.5f, 4300, 300);
        h.frame(3);
        CHECK(h.core().documentFile(0) == file);
        CHECK(other.takeIncoming().empty());
    }
    REQUIRE(h.core().saveAs(file));
    h.frame(3);
    REQUIRE_FALSE(h.core().documentModified(0));
    h.moveMouse(700, 400);
    h.frame(2);
    const auto* tab = h.find(title);
    REQUIRE(tab);
    const imgui_items::Item t = *tab;
    h.drag((t.min.x + t.max.x) * 0.5f, (t.min.y + t.max.y) * 0.5f, 4300, 300);
    h.frame(3);
    CHECK(h.core().documentFile(0).empty());  // closed here: an empty Untitled is left
    const auto got = other.takeIncoming();
    REQUIRE(got.size() == 1);
    CHECK(fs::path(got[0]) == file);
    CHECK(h.imguiErrors().empty());
    fs::remove_all(dir);
}

TEST_CASE("top search box: every Thai word names a real command") {
    Harness h;
    for (const ui::ThaiWord& w : ui::thaiWords())
        CHECK_MESSAGE(h.core().registry().find(w.command) != nullptr, std::string(w.word), " -> ",
                      w.command);
    CHECK(ui::thaiWords().size() >= 150);
}

TEST_CASE("top search box: a Thai word picks a command from the list") {
    Harness h;
    h.frame(2);
    REQUIRE(h.click("##topSearch", "MainMenuBar"));
    h.frame(2);
    h.type("วงกลม");
    h.frame(2);
    CHECK(h.windowOpen("topSearchList"));
    h.key(ImGuiKey_Enter);
    h.frame(2);
    const auto* prompt = h.core().promptInfo();
    REQUIRE(prompt != nullptr);  // CIRCLE asks for its centre
    h.core().pressEscape();
    h.frame(2);
    CHECK(!h.windowOpen("topSearchList"));
}

TEST_CASE("practice mode: the start page's New here? button opens it") {
    Harness h(1600, 900);
    h.frame(2);
    REQUIRE(h.has("Try Practice Mode", "##start"));
    REQUIRE(h.click("Try Practice Mode", "##start"));
    h.frame(3);
    CHECK(h.windowOpen("Practice"));
}

TEST_CASE("practice mode: a lesson from the Help menu, played on the command line, gets stars") {
    Harness h;
    h.frame(2);
    REQUIRE(h.click("Help", "MainMenuBar"));
    h.frame(2);
    REQUIRE(h.click("Practice Mode", "##Menu"));
    h.frame(3);
    REQUIRE(h.windowOpen("Practice"));
    CHECK(h.gui().practice().lessons().size() >= 11);
    CHECK_FALSE(h.has("2.1  Your first line", "Practice"));  // only the next lesson's world is open
    REQUIRE(h.click("2  Draw  /  วาด", "Practice"));
    h.frame(2);
    REQUIRE(h.click("2.1  Your first line", "Practice"));
    h.frame(3);
    CHECK(h.core().documentTitle(h.core().activeDocument()) == "Practice 2.1");
    REQUIRE(h.gui().practice().run() != nullptr);
    CHECK(h.gui().practice().run()->currentStep() == 0);
    CHECK(h.has("Hint", "Practice"));
    h.command("L");
    h.frame(2);
    CHECK(h.gui().practice().run()->currentStep() == 1);
    h.command("0,0");
    h.command("@1000,0");
    h.command("");
    h.frame(3);
    CHECK(h.gui().practice().finished());
    CHECK(h.gui().practice().progress().stars("2.1") == 3);
    h.captureText(true);
    h.frame(2);
    CHECK(h.shownText().find("Lesson complete!") != std::string::npos);
    CHECK(h.shownText().find("New badge: First Line") != std::string::npos);
    CHECK(h.gui().practice().progress().badges.count("first-line") == 1);
    CHECK_FALSE(h.core().modified());  // a practice drawing never asks to be saved
    CHECK(h.has("Next lesson", "Practice"));
    CHECK(h.imguiErrors().empty());
}

TEST_CASE("practice mode: a Thai word opens it, Restart and the lesson map work") {
    Harness h;
    h.frame(2);
    h.command("ฝึก");
    h.frame(3);
    REQUIRE(h.windowOpen("Practice"));
    REQUIRE(h.click("1.3  Undo and redo", "Practice"));
    h.frame(3);
    REQUIRE(h.gui().practice().run() != nullptr);
    CHECK(h.core().document().modelSpaceCount() == 2);
    CHECK_FALSE(h.core().undo());  // the starting drawing cannot be undone away
    h.command("ERASE");
    h.command("L");
    h.command("");
    h.frame(2);
    CHECK(h.gui().practice().run()->currentStep() == 1);
    REQUIRE(h.click("Restart", "Practice"));
    h.frame(3);
    CHECK(h.gui().practice().run()->currentStep() == 0);
    CHECK(h.core().document().modelSpaceCount() == 2);
    CHECK(h.core().documentCount() == 1);  // the old practice tab went
    REQUIRE(h.click("< All lessons", "Practice"));
    h.frame(2);
    CHECK(h.gui().practice().run() == nullptr);
    CHECK(h.has("1.3  Undo and redo", "Practice"));
    CHECK(h.has("Boss: Tidy up", "Practice"));  // each world ends with its boss
    CHECK(h.imguiErrors().empty());
}

TEST_CASE("practice mode: a dimension lesson from a folded world, placed by typing, gets stars") {
    Harness h;
    h.frame(2);
    h.command("PRACTICE");
    h.frame(3);
    REQUIRE(h.windowOpen("Practice"));
    CHECK(h.has("3  Modify  /  แก้ไข", "Practice"));
    CHECK(h.has("4  Layers  /  เลเยอร์", "Practice"));
    REQUIRE(h.click("5  Annotate  /  ข้อความและขนาด", "Practice"));
    h.frame(2);
    REQUIRE(h.click("5.2  Dimension a room", "Practice"));
    h.frame(3);
    REQUIRE(h.gui().practice().run() != nullptr);
    h.command("DLI");
    h.command("0,0");
    h.command("4000,0");
    h.frame(2);
    CHECK(h.gui().practice().run()->currentStep() == 1);
    h.command("2000,-700");
    h.command("ขนาดเส้นตรง");
    h.command("0,0");
    h.command("0,3500");
    h.command("-700,1750");
    h.frame(3);
    CHECK(h.gui().practice().finished());
    CHECK(h.gui().practice().progress().stars("5.2") == 3);
    CHECK(h.imguiErrors().empty());
}

TEST_CASE("toolbox: every tool runs a command the program has") {
    Harness h(1280, 720);
    std::set<std::string> seen;
    for (const ui::ToolTab& tab : ui::toolboxTabs()) {
        CHECK_FALSE(tab.groups.empty());
        for (const ui::ToolGroup& g : tab.groups) {
            for (const ui::ToolDef& t : g.tools) {
                INFO(tab.id << " / " << g.name << " / " << t.command);
                CHECK(h.core().registry().find(t.command) != nullptr);
                CHECK_FALSE(t.label.empty());
                for (const ui::ToolVariant& v : t.variants) {
                    const std::string name = v.command.substr(0, v.command.find(' '));
                    CHECK(h.core().registry().find(name) != nullptr);
                }
                seen.insert(t.command);
            }
        }
    }
    CHECK(seen.size() > 100);
}

TEST_CASE("toolbox: tabs switch, fold and keep the drawing area clear") {
    Harness h(1920, 1080);
    h.core().setSetting("ui.compact", "0");  // the full Toolbox: tabs over big labelled buttons
    h.frame(3);
    CHECK(matchesGolden(h.windowShot("##toolbox"), "toolbox_draw"));
    const float open = ImGui::GetMainViewport()->WorkPos.y;
    REQUIRE(h.click("Modify", "##toolbox"));
    h.frame(3);
    CHECK(h.core().settings().getString("ui.toolbox.tab") == "modify");
    CHECK(matchesGolden(h.windowShot("##toolbox"), "toolbox_modify"));
    h.core().setSetting("ui.toolbox.folded", "1");
    h.frame(3);
    CHECK(ImGui::GetMainViewport()->WorkPos.y < open - 40.0f);
}

// Every toolbar icon on one sheet (saved as icon-sheet.png in the build's gui-test-failures folder
// so a person can look at it) and checked for the mistakes a picture would show: an empty icon, a
// stroke outside its square, two icons drawn the same, a missing name.
TEST_CASE("icon sheet: every icon draws inside its square, and no two are alike") {
    using app::Icon;
    const int count = static_cast<int>(Icon::Count);
    REQUIRE(count >= 90);
    constexpr int cols = 12, cellW = 116, cellH = 92, iconPx = 56;
    const int rows = (count + cols - 1) / cols;
    Harness h(1440, 900);
    h.frame(2);
    std::vector<std::string> problems;
    std::map<std::size_t, std::string> seen;
    const Image sheet = h.overlayShot(cols * cellW, rows * cellH, [&](ImDrawList* dl) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(float(cols * cellW), float(rows * cellH)),
                          IM_COL32(32, 34, 38, 255));
        for (int i = 0; i < count; ++i) {
            const Icon icon = static_cast<Icon>(i);
            const std::string name = app::iconName(icon);
            if (name.empty())
                problems.push_back("icon " + std::to_string(i) + " has no name");
            const float x = float((i % cols) * cellW), y = float((i / cols) * cellH);
            const ImVec2 lo(x + (cellW - iconPx) * 0.5f, y + 6);
            dl->AddRect(lo, ImVec2(lo.x + iconPx, lo.y + iconPx), IM_COL32(60, 64, 72, 255));
            const int v0 = dl->VtxBuffer.Size;
            app::drawIcon(dl, icon, lo, float(iconPx), IM_COL32(225, 228, 235, 255),
                          app::iconAccent(app::iconGroup(icon)));
            if (dl->VtxBuffer.Size == v0) {
                problems.push_back(name + " draws nothing");
                continue;
            }
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            std::size_t hash = 1469598103934665603ull;
            for (int v = v0; v < dl->VtxBuffer.Size; ++v) {
                const ImVec2 p = dl->VtxBuffer[v].pos;
                x0 = std::min(x0, p.x), y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
                hash = (hash ^ std::size_t(std::lround((p.x - lo.x) * 4))) * 1099511628211ull;
                hash = (hash ^ std::size_t(std::lround((p.y - lo.y) * 4))) * 1099511628211ull;
            }
            const float slack = 0.06f * iconPx;  // strokes and accent marks may overshoot a little
            if (x0 < lo.x - slack || y0 < lo.y - slack || x1 > lo.x + iconPx + slack ||
                y1 > lo.y + iconPx + slack)
                problems.push_back(name + " leaves its square");
            if (x1 - x0 < 0.3f * iconPx || y1 - y0 < 0.3f * iconPx)
                problems.push_back(name + " is too small");
            const auto [it, fresh] = seen.emplace(hash, name);
            if (!fresh)
                problems.push_back(name + " looks the same as " + it->second);
            const char* label = name.c_str();
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(x + (cellW - ts.x) * 0.5f, lo.y + iconPx + 6),
                        IM_COL32(180, 186, 198, 255), label);
        }
    });
    CHECK(!sheet.empty());
    const auto file = savePreview(sheet, "icon-sheet");
    MESSAGE("icon sheet: " << file.string());
    for (const std::string& p : problems)
        MESSAGE("icon problem: " << p);
    CHECK(problems.empty());
    CHECK(h.imguiErrors().empty());
}

TEST_CASE("toolbox: Recent group fills as commands run, and Quick rail buttons run their command") {
    Harness h(1920, 1080);
    h.core().setSetting("ui.toolbox.showRecent", "1");
    h.frame(3);
    CHECK(h.core().settings().getStrings("ui.toolbox.recent").empty());
    h.core().submitText("CIRCLE");
    h.frame(3);
    h.core().pressEscape();
    h.core().submitText("LINE");
    h.frame(3);
    h.core().pressEscape();
    h.frame(3);
    const auto recent = h.core().settings().getStrings("ui.toolbox.recent");
    REQUIRE(recent.size() == 2);
    CHECK(recent[0] == "LINE");
    CHECK(recent[1] == "CIRCLE");
    CHECK(matchesGolden(h.windowShot("##toolbox"), "toolbox_recent"));

    // The rail: six default buttons; clicking one starts its command.
    int rail = 0;
    while (h.has("##tool", "##quickrail") && h.find("##tool", "##quickrail", rail))
        ++rail;
    CHECK(rail == 6);
    REQUIRE(h.click("##tool", "##quickrail", 3));  // Move
    h.frame(3);
    CHECK(runningName(h) == "MOVE");
    h.core().pressEscape();
    h.frame(2);
    CHECK(matchesGolden(h.windowShot("##quickrail"), "quickrail"));
}

TEST_CASE("toolbox: a tool in Recent and in its own group both work after selecting first") {
    Harness h(1920, 1080);
    threeLayers(h);
    h.core().setSetting("ui.toolbox.tab", "modify");
    h.core().setSetting("ui.toolbox.showRecent", "1");
    h.core().selectAll();
    h.core().submitText("ROTATE");
    h.frame(3);
    h.core().pressEscape();
    h.frame(3);
    // Recent holds Rotate once; Rotate is also the third button of Move & copy.
    for (const int nth : {0, 3}) {
        h.core().selectAll();
        h.frame(3);
        REQUIRE(h.click("##tool", "##toolbox", nth));
        h.frame(3);
        CHECK(runningName(h) == "ROTATE");
        h.core().pressEscape();
        h.frame(3);
    }
    CHECK(h.imguiErrors().empty());
}

TEST_CASE(
    "mirror: typed command leaves the command line empty, Yes/No click, Space takes the default") {
    Harness h(1920, 1080);
    threeLayers(h);
    for (const int how : {0, 1, 2}) {
        CAPTURE(how);
        h.core().selectAll();
        REQUIRE(h.click("##command", "Command"));
        h.type("mi");
        if (how == 1)
            h.type(" ");  // Space ends the command name like Enter
        else
            h.key(ImGuiKey_Enter);
        h.frame(3);
        REQUIRE(runningName(h) == "MIRROR");
        h.type("0,0");
        h.key(ImGuiKey_Enter);
        h.frame(2);
        h.type("10,0");
        h.key(ImGuiKey_Enter);
        h.frame(3);
        const auto* p = h.core().runner().prompt();
        REQUIRE(p);
        CHECK(p->text.find("Erase") != std::string::npos);
        if (how == 0) {
            REQUIRE(h.click("Yes"));
        } else if (how == 1) {
            REQUIRE(h.click("No"));
        } else {
            h.type(" ");
        }
        h.frame(3);
        CHECK_FALSE(h.core().runner().active());
        h.core().pressEscape();
        h.frame(2);
    }
    CHECK(h.imguiErrors().empty());
}

TEST_CASE("toolbox: no two buttons share an ImGui ID on any tab, with Recent shown") {
    Harness h(1920, 1080);
    h.core().setSetting("ui.toolbox.showRecent", "1");
    h.frame(3);
    for (const char* cmd : {"ROTATE", "MOVE", "CIRCLE", "LINE", "COPY", "MIRROR"}) {
        h.core().submitText(cmd);
        h.frame(2);
        h.core().pressEscape();
        h.frame(2);
    }
    std::vector<std::string> clashes;
    for (const char* tab : {"draw", "modify", "annotate", "blocks", "inspect", "output"}) {
        for (const char* expert : {"0", "1"}) {
            h.core().setSetting("ui.toolbox.tab", tab);
            h.core().setSetting("ui.toolbox.expert", expert);
            h.frame(4);
            std::map<ImGuiID, std::string> seen;
            for (const auto& it : tbcad::imgui_items::items()) {
                if (it.window.find("toolbox") == std::string::npos &&
                    it.window.find("quickrail") == std::string::npos &&
                    it.window.find("MainMenuBar") == std::string::npos)
                    continue;
                const auto [pos, fresh] = seen.emplace(it.id, it.window + "/" + it.label);
                if (!fresh)
                    clashes.push_back(std::string(tab) + ": " + pos->second + " and " + it.window +
                                      "/" + it.label);
            }
        }
    }
    for (const std::string& c : clashes)
        MESSAGE("ID clash: " << c);
    CHECK(clashes.empty());
}

TEST_CASE("menu bar: drawing menus sit under Commands, Classic Menus brings them back") {
    Harness h(1920, 1080);
    h.frame(3);
    CHECK(h.has("Commands", "MainMenuBar"));
    CHECK_FALSE(h.has("Draw", "MainMenuBar"));
    REQUIRE(h.click("Commands", "MainMenuBar"));
    h.frame(3);
    CHECK(h.has("Draw", "##Menu"));
    CHECK(h.has("Modify", "##Menu"));
    h.key(ImGuiKey_Escape);
    h.frame(2);
    h.core().setSetting("ui.compactMenus", "0");
    h.frame(3);
    CHECK_FALSE(h.has("Commands", "MainMenuBar"));
    CHECK(h.has("Draw", "MainMenuBar"));
}

TEST_CASE("toolbox: Expert mode is icons only and shorter, tooltips teach the keyboard way") {
    Harness h(1920, 1080);
    h.core().setSetting("ui.compact", "0");
    h.frame(3);
    const float beginner = ImGui::GetMainViewport()->WorkPos.y;
    h.core().setSetting("ui.toolbox.expert", "1");
    h.frame(3);
    CHECK(ImGui::GetMainViewport()->WorkPos.y < beginner - 15.0f);
    CHECK(matchesGolden(h.windowShot("##toolbox"), "toolbox_expert"));
    // Hover the first Toolbox button: the tip names the command, its alias and the typed way.
    h.core().setSetting("ui.toolbox.expert", "0");
    h.frame(3);
    const auto* first = h.find("##tool", "##toolbox", 0);
    REQUIRE(first);
    h.moveMouse((first->min.x + first->max.x) * 0.5f, (first->min.y + first->max.y) * 0.5f);
    h.frame(60);
    const Image tip = h.windowShot("##Tooltip");
    REQUIRE_FALSE(tip.empty());
    CHECK(matchesGolden(tip, "toolbox_tooltip"));
}

TEST_CASE("toolbox: a narrow window gets scroll arrows that move the row") {
    Harness h(900, 700);
    h.core().setSetting("ui.compact", "0");
    h.frame(4);
    ImGuiWindow* w = ImGui::FindWindowByName("##toolbox");
    REQUIRE(w);
    CHECK(w->ScrollMax.x > 0.0f);
    CHECK(w->Scroll.x == 0.0f);
    const float winRight = w->Pos.x + w->Size.x;
    h.click(winRight - 8.0f, w->Pos.y + 70.0f);
    h.frame(3);
    CHECK(w->Scroll.x > 100.0f);
    CHECK(matchesGolden(h.windowShot("##toolbox"), "toolbox_narrow"));
}

TEST_CASE("ARRAY: Toolbox variants start their array type, option buttons change the settings") {
    Harness h(1440, 900);
    h.frame(3);
    h.core().submitText("RECTANGLE 0,0 10,10");
    h.core().pressEscape();
    h.frame(3);
    auto count = [&] {
        std::size_t n = 0;
        h.core().document().forEachEntity([&](const Entity&) { ++n; });
        return n;
    };
    for (const ui::ToolTab& tab : ui::toolboxTabs())
        for (const ui::ToolGroup& g : tab.groups)
            for (const ui::ToolDef& t : g.tools)
                for (const ui::ToolVariant& v : t.variants) {
                    CAPTURE(v.command);
                    h.core().submitText(v.command);
                    h.core().submitText("all");
                    h.core().submitText("");
                    h.frame(2);
                    // The command must have moved on to its own questions, not stay on selection.
                    if (v.command.rfind("ARRAY", 0) == 0) {
                        CHECK(h.core().promptText().find("Select objects") == std::string::npos);
                        CHECK(h.core().runner().active());
                    }
                    h.core().pressEscape();
                    h.core().pressEscape();
                }
    h.core().submitText("ARRAYRECT");
    h.core().submitText("all");
    h.core().submitText("");
    h.frame(3);
    REQUIRE(h.has("Rows", "optionbar"));
    REQUIRE(h.click("Rows", "optionbar"));
    h.frame(3);
    h.command("5");
    h.frame(3);
    REQUIRE(h.has("Accept", "optionbar"));
    CHECK(h.core().promptText().find("5 rows") != std::string::npos);
    REQUIRE(h.click("Accept", "optionbar"));
    h.frame(3);
    CHECK(count() >= 1);
    CHECK(h.core().history().size() > 0);
}

TEST_CASE("DONUT: from the Toolbox with Enter, Space and clicks, diameters picked as two points") {
    Harness h(1920, 1080);
    h.core().setSetting("drafting.osnap", "0");
    h.core().setSetting("ui.toolbox.tab", "draw");
    h.frame(3);
    auto rings = [&] {
        std::vector<PolylineData> out;
        h.core().document().forEachEntity([&](const Entity& e) {
            if (const auto* p = std::get_if<PolylineData>(&e.data))
                out.push_back(*p);
        });
        return out;
    };
    const app::ViewportRect vp = h.gui().lastViewportRect();
    const float cx = vp.x + vp.w * 0.5f, cy = vp.y + vp.h * 0.5f;

    // The Donut button (Lines has six tools, then Circle, Arc, Ellipse), Enter and Space take the
    // remembered diameters, a click places a ring, Enter ends.
    REQUIRE(h.click("##tool", "##toolbox", 9));
    h.frame(3);
    REQUIRE(runningName(h) == "DONUT");
    h.key(ImGuiKey_Enter);
    h.frame(2);
    h.key(ImGuiKey_Space);
    h.frame(2);
    CHECK(h.core().promptText().find("center of donut") != std::string::npos);
    h.click(cx, cy);
    h.frame(2);
    h.click(cx + 200, cy);
    h.frame(2);
    h.key(ImGuiKey_Enter);
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    REQUIRE(rings().size() == 2);
    CHECK(rings()[0].constantWidth == doctest::Approx(2.5));

    // Typed DO; each diameter answered by two clicks in the drawing, as in AutoCAD.
    auto world = [&](float x, float y) {
        return h.core().camera().screenToWorld(
            {x - ImGui::GetMainViewport()->WorkPos.x, y - ImGui::GetMainViewport()->WorkPos.y});
    };
    h.command("DO");
    h.frame(3);
    h.click(cx - 300, cy + 100);
    h.frame(2);
    CHECK(h.core().promptText().find("second point") != std::string::npos);
    h.click(cx - 280, cy + 100);
    h.frame(2);
    CHECK(h.core().promptText().find("outside diameter") != std::string::npos);
    h.click(cx - 300, cy + 150);
    h.frame(2);
    h.click(cx - 240, cy + 150);
    h.frame(2);
    CHECK(h.core().promptText().find("center of donut") != std::string::npos);
    h.click(cx - 100, cy - 100);
    h.frame(2);
    h.key(ImGuiKey_Escape);
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    REQUIRE(rings().size() == 3);
    const double inner = distance(world(cx - 300, cy + 100), world(cx - 280, cy + 100));
    const double outer = distance(world(cx - 300, cy + 150), world(cx - 240, cy + 150));
    CHECK(rings()[2].constantWidth == doctest::Approx((outer - inner) / 2).epsilon(0.01));

    // Typed points work the same way, and Enter at the second point goes back to the question.
    h.command("DO");
    h.frame(2);
    h.command("0,0");
    CHECK(h.core().promptText().find("second point") != std::string::npos);
    h.command("");
    CHECK(h.core().promptText().find("inside diameter") != std::string::npos);
    h.command("0,0");
    h.command("6,8");  // 10 apart
    h.command("@30<0");
    h.command("30");
    h.command("500,500");
    h.command("");
    h.frame(3);
    REQUIRE(rings().size() == 4);
    CHECK(rings()[3].constantWidth == doctest::Approx(10));
    CHECK(h.gui().commandLineText().empty());
}

namespace {
// The Toolbox's tool strip in the compact layout: a child window of "##toolbox".
ImGuiWindow* toolRow() {
    for (ImGuiWindow* w : GImGui->Windows)
        if (w->WasActive && std::string_view(w->Name).find("##toolrow") != std::string_view::npos)
            return w;
    return nullptr;
}
}  // namespace

TEST_CASE("compact layout: thin bars, a big drawing area, options in the command line") {
    Harness h(1920, 1080);
    h.frame(3);
    // One row of tabs and icons; the File / Edit buttons sit in the menu bar; no Pan / Zoom
    // buttons over the drawing.
    ImGuiWindow* box = ImGui::FindWindowByName("##toolbox");
    REQUIRE(box);
    CHECK(box->Size.y < 40.0f);
    CHECK_FALSE(h.windowOpen("##toolstrip"));
    CHECK_FALSE(h.windowOpen("##navbar"));
    CHECK(h.has("##tool", "##MainMenuBar"));
    REQUIRE(toolRow());
    // The drawing gets most of the window.
    ImGuiDockNode* centre = ImGui::DockBuilderGetCentralNode(0x7BC4D001u);
    REQUIRE(centre);
    CHECK(centre->Size.x > 1920.0f * 0.75f);
    CHECK(centre->Size.y > 1080.0f * 0.78f);
    CHECK(matchesGolden(h.windowShot("##toolbox"), "toolbox_compact"));

    // A point prompt with options: the box by the cursor holds no buttons, the command line's
    // option buttons work (a click there used to read a prompt it had just replaced).
    h.core().submitText("PLINE");
    h.core().submitText("0,0");
    h.moveMouse(800, 500);
    h.frame(5);
    CHECK_FALSE(h.has("Arc", "##hud"));
    REQUIRE(h.click("Arc", "Command"));
    h.frame(3);
    REQUIRE(h.core().runner().prompt());
    CHECK(h.core().promptText().find("arc") != std::string::npos);
    REQUIRE(h.click("Line", "Command"));
    h.frame(3);
    CHECK(h.core().promptText().find("next point") != std::string::npos);
    h.core().pressEscape();
    h.frame(2);
}

TEST_CASE("compact layout: a narrow window scrolls the tool row, the tabs stay") {
    Harness h(900, 700);
    h.frame(4);
    ImGuiWindow* row = toolRow();
    REQUIRE(row);
    CHECK(row->ScrollMax.x > 0.0f);
    CHECK(row->Scroll.x == 0.0f);
    h.click(row->Pos.x + row->Size.x - 8.0f, row->Pos.y + row->Size.y * 0.5f);
    h.frame(3);
    CHECK(row->Scroll.x > 100.0f);
    CHECK(h.has("Output", "##toolbox"));
}

// Test.8 report "ARRAY does not work, its settings cannot be changed": the grid did not show
// while its options were set (only text), and a finished array had no settings in Properties.
TEST_CASE("ARRAY from the Toolbox shows the grid while it is set up, Properties changes it") {
    Harness h(1600, 1000);
    h.core().setSetting("ui.toolbox.tab", "modify");
    h.core().submitText("CIRCLE 0,0 10");
    h.core().pressEscape();
    h.core().zoomExtents();
    h.core().submitText("ZOOM 0.3X");
    h.frame(3);
    auto overlayVertices = [&] {
        render::Overlay overlay;
        h.core().buildOverlay(overlay);
        return overlay.build(h.core().camera()).vertices.size();
    };
    auto theArray = [&]() -> const ArrayInfo* {
        const ArrayInfo* found = nullptr;
        h.core().document().forEachEntity([&](const Entity& e) {
            if (const auto* in = entityAs<InsertData>(e); in && in->array)
                found = &*in->array;
        });
        return found;
    };
    // Start from the Toolbox button, pick with a window, finish the selection with Enter.
    int nth = -1, i = 0;
    for (const ui::ToolTab& t : ui::toolboxTabs())
        if (t.id == "modify")
            for (const ui::ToolGroup& g : t.groups)
                for (const ui::ToolDef& d : g.tools) {
                    if (d.command == "ARRAY")
                        nth = i;
                    ++i;
                }
    REQUIRE(nth >= 0);
    REQUIRE(h.click("##tool", "##toolbox", nth));
    h.frame(3);
    const app::ViewportRect vp = h.gui().lastViewportRect();
    h.drag(vp.x + 100, vp.y + 100, vp.x + vp.w - 100, vp.y + vp.h - 100);
    h.frame(3);
    h.key(ImGuiKey_Enter);
    h.frame(3);
    REQUIRE(h.has("Rectangular", "optionbar"));
    const std::size_t before = overlayVertices();
    h.key(ImGuiKey_Enter);  // <Rectangular>
    h.frame(3);
    REQUIRE(h.has("Accept", "optionbar"));
    CHECK(overlayVertices() > before + 100);  // the 3 x 4 grid is drawn before it is accepted
    REQUIRE(h.click("Rows", "optionbar"));
    h.frame(3);
    h.command("5");
    h.frame(3);
    CHECK(h.core().promptText().find("5 rows") != std::string::npos);
    h.key(ImGuiKey_Enter);  // <Accept>
    h.frame(3);
    CHECK_FALSE(h.core().runner().active());
    const ArrayInfo* a = theArray();
    REQUIRE(a);
    CHECK(a->rows == 5);
    CHECK(a->columns == 4);

    // Select it and change the rows and the column spacing in Object Properties.
    h.core().selectAll();
    h.frame(3);
    REQUIRE(h.click("Object Properties", "DockSpace"));
    h.frame(3);
    // Geometry boxes: position X, Y, scale X, Y, rotation, then the array's columns, column
    // spacing, rows, row spacing and angle.
    int boxes = 0;
    while (h.find("##v", "Object Properties", boxes))
        ++boxes;
    REQUIRE(boxes == 10);
    const int box = 7;
    REQUIRE(h.enterText("##v", "2", "Object Properties", box));
    h.frame(3);
    a = theArray();
    REQUIRE(a);
    CHECK(a->rows == 2);
    CHECK(a->columns == 4);
    // One undo brings the 5 rows back.
    h.core().submitText("U");
    h.frame(3);
    a = theArray();
    REQUIRE(a);
    CHECK(a->rows == 5);
}
