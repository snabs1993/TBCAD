// Automated GUI tests: the real Gui is driven with queued mouse and keyboard events, without a
// window or a GPU (see gui_harness.h). They stand in for a person clicking through the program,
// so a panel that compiles but does nothing, or draws wrongly, fails here.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "gui_harness.h"
#include "tbcad/layers/layer_states.h"
#include "tbcad/render/overlay.h"
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

namespace {

// Where a world point is in the harness window (the viewport starts below the menu and tabs).
ImVec2 screenOf(Harness& h, Vec2 world) {
    const Vec2 s = h.core().camera().worldToScreen(world);
    return ImVec2(static_cast<float>(s.x), static_cast<float>(s.y) + 92.0f);
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
    CHECK(matchesGolden(crop(h.screenshot(), 60, 820, 700, 230), "command_suggest"));
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
          P{"laywalk", "Layer Walk"}, P{"text_editor", "Text"}}) {
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
    const ImVec2 onLine = screenOf(h, {180, 100});  // away from the midpoint grip
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
    CHECK(matchesGolden(h.windowShot("Command Help"), "command_help_circle"));
    REQUIRE(h.click("Close", "Command Help"));
    h.frame(3);
    CHECK_FALSE(h.windowOpen("Command Help"));
    CHECK(h.core().runner().active());  // the command itself keeps waiting
}

TEST_CASE("hover tooltip, right-click as Enter and the cursor HUD switch") {
    Harness h(1920, 1080);
    h.core().setSetting("drafting.osnap", "0");
    h.frame(3);
    h.command("LINE");
    const ImVec2 a = screenOf(h, {100, 100}), b = screenOf(h, {400, 100});
    h.click(a.x, a.y);
    h.click(b.x, b.y);
    h.core().pressEscape();
    h.core().zoomExtents();
    h.frame(20);
    h.scroll(700, 500, -3.0f);
    h.frame(30);
    REQUIRE(entityCount(h) == 1);
    // (a pixel up: the viewport starts on a fraction of a pixel, and the pick aperture is tight)
    ImVec2 onLine = screenOf(h, {180, 100});
    onLine.y -= 1.0f;

    // Off by default: resting on the line shows nothing. Set as text, like the SETTING command.
    h.moveMouse(onLine.x, onLine.y);
    h.frame(60);
    CHECK(h.windowShot("##Tooltip").empty());
    h.core().setSetting("hover.tooltip", "1");
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

    // Isolate Layer from the menu hides the other layers.
    REQUIRE(h.click("Isolate Layer", "Popup"));
    h.frame(3);
    h.core().pressEscape();
    CHECK_FALSE(layer(h, "A-Walls")->on);
    CHECK_FALSE(layer(h, "A-Doors")->on);
    CHECK(layer(h, "0")->on);
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
    CHECK(order() == "One,Two,");
    REQUIRE(h.rightClick("Two", "##layouttabs"));
    h.frame(2);
    REQUIRE(h.click("Move left", "Popup"));
    h.frame(5);
    CHECK(order() == "Two,One,");
    REQUIRE(h.rightClick("Two", "##layouttabs"));
    h.frame(2);
    REQUIRE(h.click("Copy", "Popup"));
    h.frame(5);
    CHECK(h.core().layoutNames().size() == 3);
    CHECK(h.has("Two (2)", "##layouttabs"));
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

TEST_CASE("every menu item can be clicked without trouble") {
    std::vector<std::string> tops;
    {
        Harness h(1920, 1080);
        h.frame(3);
        for (const auto& it : imgui_items::items())
            if (it.window.find("MainMenuBar") != std::string::npos && !it.label.empty())
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

TEST_CASE("tool strip: every button starts its command or action") {
    int total = 0;
    {
        Harness h(1920, 1080);
        h.frame(3);
        while (h.has("##tool", "##toolstrip") && h.find("##tool", "##toolstrip", total))
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
        REQUIRE(h.click("##tool", "##toolstrip", i));
        h.frame(4);
        if (h.core().runner().active()) {
            ++started;
            h.core().pressEscape();
        }
        h.key(ImGuiKey_Escape);  // a file picker or dialog
        h.frame(2);
    }
    // Draw and Modify tools run commands: at least those 17 are waiting for input after a click.
    CHECK(started >= 15);
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
    REQUIRE(h.has("Manager...", "Layers"));
    REQUIRE(h.click("Manager...", "Layers"));
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
    REQUIRE(h.click("Translate...", "Layers"));
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
    for (const char* opener : {"Manager...", "Translate..."}) {
        const std::string win =
            std::string(opener) == "Manager..." ? "Layer States Manager" : "Layer Translator";
        Harness h(1920, 1080);
        threeLayers(h);
        REQUIRE(h.click(opener, "Layers"));
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
    REQUIRE(h.enterText("##Linear factor", "2", "Dimension Styles"));
    h.frame(3);
    CHECK(style().linearFactor == doctest::Approx(2.0));

    REQUIRE(h.click("##Fit", "Dimension Styles"));
    h.frame(2);
    REQUIRE(h.click("Text first", "Combo"));
    h.frame(3);
    CHECK(style().fit == DimFit::TextFirst);

    REQUIRE(h.click("##Dragged text", "Dimension Styles"));
    h.frame(2);
    REQUIRE(h.click("Move freely", "Combo"));
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
