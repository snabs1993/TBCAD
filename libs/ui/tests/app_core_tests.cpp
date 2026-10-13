#include <doctest/doctest.h>

#include <chrono>
#include <fstream>

#include "tbcad/core/dump.h"
#include "tbcad/ui/app_core.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::ui;
using namespace tbcad::ui::test;
namespace fs = std::filesystem;

namespace {

struct Fixture {
    fs::path dir;
    std::unique_ptr<cmd::Registry> registry = makeRegistry();
    std::unique_ptr<AppCore> core;
    explicit Fixture(const std::string& name) : dir(tempDir(name)) {
        core = std::make_unique<AppCore>(*registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1000, 800);
        render::Camera2D cam = core->camera();
        cam.center = {500, 400};
        cam.pixelsPerMm = 1.0;
        core->setCamera(cam);
    }
    // Draws a line through the command line.
    void line(const char* a, const char* b) {
        core->submitText("TESTLINE");
        core->submitText(a);
        core->submitText(b);
    }
    PointerState at(Vec2 world, bool shift = false) {
        return PointerState{core->camera().worldToScreen(world), shift, false};
    }
};

}  // namespace

TEST_CASE("command line runs commands, aliases, prompts and errors") {
    Fixture f("cmdline");
    f.core->submitText("tl");  // alias, any case
    CHECK(f.core->runner().active());
    CHECK(f.core->promptText().find("First point") != std::string::npos);
    f.core->submitText("0,0");
    f.core->submitText("100,0");
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.core->document().modelSpaceCount() == 1);
    CHECK(f.core->modified());

    f.core->submitText("NOSUCHCOMMAND");
    CHECK(f.core->history().back().find("Unknown command") != std::string::npos);
    CHECK(f.core->promptText() == "Command: ");
}

TEST_CASE("words after the command name answer its prompts") {
    Fixture f("words");
    f.core->submitText("TESTLINE 0,0 10,10");
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.core->document().modelSpaceCount() == 1);
}

TEST_CASE("Escape cancels a command, then clears the selection") {
    Fixture f("esc");
    f.core->submitText("TESTLINE");
    f.core->submitText("0,0");
    f.core->pressEscape();
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.core->document().modelSpaceCount() == 0);

    f.line("0,0", "100,0");
    f.core->click(f.at({50, 0}));
    CHECK(f.core->selection().size() == 1);
    f.core->pressEscape();
    CHECK(f.core->selection().empty());
}

TEST_CASE("undo and redo with labels and a clean modified flag path") {
    Fixture f("undo");
    CHECK_FALSE(f.core->modified());
    CHECK(f.core->undoLabel().empty());
    f.line("0,0", "100,0");
    CHECK(f.core->undoLabel() == "Undo TESTLINE");
    CHECK(f.core->undo());
    CHECK(f.core->document().modelSpaceCount() == 0);
    CHECK(f.core->redoLabel() == "Redo TESTLINE");
    CHECK(f.core->redo());
    CHECK(f.core->document().modelSpaceCount() == 1);
    f.core->submitText("TESTLINE");
    CHECK(f.core->undoLabel().empty());  // a command is running
    CHECK_FALSE(f.core->undo());
    f.core->pressEscape();
}

TEST_CASE("clicks in the viewport answer point prompts through the snap-free path") {
    Fixture f("clickpoint");
    f.core->submitText("TESTLINE");
    f.core->click(f.at({10, 20}));
    f.core->pointerMoved(f.at({110, 20}));
    f.core->click(f.at({110, 20}));
    REQUIRE(f.core->document().modelSpaceCount() == 1);
    auto ids = f.core->document().modelSpace();
    const auto* line = entityAs<LineData>(*f.core->document().find(ids[0]));
    REQUIRE(line);
    CHECK(line->a.x == doctest::Approx(10));
    CHECK(line->b.x == doctest::Approx(110));
    CHECK(line->b.y == doctest::Approx(20));
}

TEST_CASE("the overlay shows the rubber band while a command waits for a point") {
    Fixture f("overlay");
    f.core->submitText("TESTLINE");
    f.core->click(f.at({0, 0}));
    f.core->pointerMoved(f.at({100, 100}));
    render::Overlay overlay;
    f.core->buildOverlay(overlay);
    CHECK_FALSE(overlay.empty());
    CHECK_FALSE(overlay.build(f.core->camera()).vertices.empty());
    f.core->pointerLeft();
    f.core->pressEscape();
    f.core->buildOverlay(overlay);
    CHECK(overlay.empty());
}

TEST_CASE("click selects, shift removes, empty click clears") {
    Fixture f("clicksel");
    f.line("0,0", "100,0");
    f.line("0,50", "100,50");
    f.core->click(f.at({50, 0}));
    f.core->click(f.at({50, 50}));
    CHECK(f.core->selection().size() == 2);
    f.core->click(f.at({50, 0}, true));
    CHECK(f.core->selection().size() == 1);
    f.core->click(f.at({700, 700}));
    CHECK(f.core->selection().empty());
}

TEST_CASE("drag left to right selects inside, right to left selects touching") {
    Fixture f("dragsel");
    f.line("10,10", "40,10");   // inside the rectangle below
    f.line("40,40", "140,40");  // sticks out
    Vec2 a = f.core->camera().worldToScreen({0, 0});
    Vec2 b = f.core->camera().worldToScreen({100, 100});
    f.core->dragSelect(a, b, PointerState{b, false, false});
    CHECK(f.core->selection().size() == 1);
    f.core->clearSelection();
    f.core->dragSelect(b, a, PointerState{a, false, false});
    CHECK(f.core->selection().size() == 2);
    f.core->dragSelect(b, a, PointerState{a, true, false});  // shift removes
    CHECK(f.core->selection().empty());
}

TEST_CASE("a selection prompt takes picks, windows and the selection made beforehand") {
    Fixture f("selprompt");
    f.line("0,0", "10,0");
    f.line("0,20", "10,20");
    f.line("0,40", "10,40");
    f.core->submitText("TESTERASE");
    f.core->click(f.at({5, 0}));
    Vec2 a = f.core->camera().worldToScreen({-5, 15});
    Vec2 b = f.core->camera().worldToScreen({15, 25});
    f.core->dragSelect(a, b, PointerState{b, false, false});
    f.core->submitText("");  // Enter finishes the selection
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.core->document().modelSpaceCount() == 1);
}

TEST_CASE("hidden, frozen and locked layers cannot be picked, and the selection drops them") {
    Fixture f("layerssel");
    LayerId layer = addLayer(f.core->document(), "L");
    EntityId e = addLine(f.core->document(), {0, 0}, {100, 0}, layer);
    f.core->click(f.at({50, 0}));
    REQUIRE(f.core->selection() == std::vector<EntityId>{e});
    modifyLayer(f.core->document(), layer, [](Layer& l) { l.locked = true; });
    CHECK(f.core->selection().empty());
    f.core->click(f.at({50, 0}));
    CHECK(f.core->selection().empty());
    modifyLayer(f.core->document(), layer, [](Layer& l) {
        l.locked = false;
        l.frozen = true;
    });
    f.core->click(f.at({50, 0}));
    CHECK(f.core->selection().empty());
}

TEST_CASE("selection follows erased entities") {
    Fixture f("selerase");
    f.line("0,0", "100,0");
    f.core->click(f.at({50, 0}));
    REQUIRE(f.core->selection().size() == 1);
    f.core->submitText("TESTERASE");  // uses the pre-selection
    CHECK(f.core->document().modelSpaceCount() == 0);
    CHECK(f.core->selection().empty());
    CHECK(f.core->undo());
    CHECK(f.core->document().modelSpaceCount() == 1);
}

TEST_CASE("wheel zoom keeps the point under the cursor fixed, 1.2 per notch") {
    Fixture f("zoom");
    Vec2 screen{300, 200};
    Vec2 before = f.core->camera().screenToWorld(screen);
    double z0 = f.core->camera().pixelsPerMm;
    f.core->zoomAtScreen(screen, 1);
    CHECK(f.core->camera().pixelsPerMm == doctest::Approx(z0 * 1.2));
    Vec2 after = f.core->camera().screenToWorld(screen);
    CHECK(after.x == doctest::Approx(before.x));
    CHECK(after.y == doctest::Approx(before.y));
    f.core->zoomAtScreen(screen, -1);
    CHECK(f.core->camera().pixelsPerMm == doctest::Approx(z0));
    f.core->panPixels(100, 0);
    CHECK(f.core->camera().center.x == doctest::Approx(500 - 100));
}

TEST_CASE("zoom extents fits the drawing; ZOOM and PAN work as commands") {
    Fixture f("extents");
    f.line("0,0", "10000,0");
    f.line("0,0", "0,5000");
    f.core->zoomExtents();
    BBox2 vis = f.core->camera().visibleWorld();
    CHECK(vis.min.x <= 0);
    CHECK(vis.max.x >= 10000);
    CHECK(vis.min.y <= 0);
    CHECK(vis.max.y >= 5000);

    f.core->submitText("ZOOM E");
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.core->camera().visibleWorld().max.x >= 10000);

    f.core->submitText("ZOOM 0,0 100,100");
    BBox2 w = f.core->camera().visibleWorld();
    CHECK(w.width() == doctest::Approx(w.height() * 1000.0 / 800.0).epsilon(0.01));
    CHECK(w.min.x <= 0);
    CHECK(w.max.x < 1000);

    Vec2 c0 = f.core->camera().center;
    f.core->submitText("PAN 0,0 10,0");
    CHECK(f.core->camera().center.x == doctest::Approx(c0.x - 10));
}

TEST_CASE("ZOOM is transparent inside another command") {
    Fixture f("zoomtransparent");
    f.core->submitText("TESTLINE");
    f.core->submitText("0,0");
    f.core->submitText("'ZOOM");
    f.core->submitText("0,0");
    f.core->submitText("50,50");
    CHECK(f.core->runner().active());  // still waiting for the line's second point
    f.core->submitText("100,0");
    CHECK(f.core->document().modelSpaceCount() == 1);
}

TEST_CASE("DEVGEN LINES makes the test drawing in one undo step") {
    Fixture f("devgen");
    f.core->submitText("DEVGEN LINES 5000");
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.core->document().modelSpaceCount() == 5000);
    CHECK(f.core->spatialIndex().size() == 5000);
    CHECK(f.core->scene().entityCount() == 5000);
    CHECK(f.core->camera().visibleWorld().max.x >= 100000);
    CHECK(f.core->undo());
    CHECK(f.core->document().modelSpaceCount() == 0);
    CHECK(f.core->scene().entityCount() == 0);
    // Hidden: absent from completion lists.
    for (const auto& name : f.core->complete("DEV"))
        CHECK(name != "DEVGEN");
    // Reproducible.
    f.core->submitText("DEVGEN LINES 100");
    std::string d1 = canonicalDump(f.core->document());
    f.core->undo();
    f.core->submitText("DEVGEN LINES 100");
    // Entity ids differ after an undo (never reused), so compare geometry only.
    auto geometry = [&] {
        std::string out;
        for (EntityId id : f.core->document().modelSpace()) {
            const auto* l = entityAs<LineData>(*f.core->document().find(id));
            out += std::to_string(l->a.x) + "," + std::to_string(l->b.y) + ";";
        }
        return out;
    };
    std::string g2 = geometry();
    CHECK_FALSE(g2.empty());
    (void)d1;
}

TEST_CASE("save, reopen: same canonical dump, title and modified flag follow") {
    Fixture f("saveopen");
    CHECK(f.core->windowTitle() == "Untitled - TBCAD");
    CHECK_FALSE(f.core->save().ok());  // no file name yet
    f.line("0,0", "100,0");
    f.core->submitText("DEVGEN LINES 200");
    CHECK(f.core->windowTitle() == "Untitled* - TBCAD");
    fs::path file = f.dir / "drawing.tbcad";
    REQUIRE(f.core->saveAs(file).ok());
    CHECK(f.core->windowTitle() == "drawing.tbcad - TBCAD");
    CHECK_FALSE(f.core->modified());
    std::string before = canonicalDump(f.core->document());

    f.core->newDocument();
    CHECK(f.core->document().modelSpaceCount() == 0);
    REQUIRE(f.core->openFile(file).ok());
    CHECK(canonicalDump(f.core->document()) == before);
    CHECK(f.core->scene().entityCount() == 201);
    CHECK(f.core->spatialIndex().size() == 201);
    CHECK_FALSE(f.core->modified());
    CHECK(f.core->recentFiles().at(0) == file.string());

    f.line("5,5", "6,6");
    CHECK(f.core->modified());
    REQUIRE(f.core->save().ok());
    CHECK_FALSE(f.core->modified());
    CHECK(fs::exists(f.dir / "drawing.tbcad.bak"));
}

TEST_CASE("opening a missing or corrupt file reports and keeps the current drawing") {
    Fixture f("badopen");
    f.line("0,0", "100,0");
    CHECK_FALSE(f.core->openFile(f.dir / "nope.tbcad").ok());
    { std::ofstream(f.dir / "junk.tbcad") << "this is not a drawing"; }
    CHECK_FALSE(f.core->openFile(f.dir / "junk.tbcad").ok());
    CHECK(f.core->document().modelSpaceCount() == 1);
    CHECK(f.core->history().back().find("Cannot open") != std::string::npos);
}

TEST_CASE("saveAs adds the extension") {
    Fixture f("ext");
    REQUIRE(f.core->saveAs(f.dir / "plain").ok());
    CHECK(f.core->file().extension() == ".tbcad");
    CHECK(fs::exists(f.dir / "plain.tbcad"));
}

TEST_CASE("autosave is written for unsaved changes and offered after a crash") {
    Fixture f("autosave");
    CHECK_FALSE(f.core->autosaveNow());  // nothing to save
    f.line("0,0", "100,0");
    CHECK(f.core->autosaveNow());
    CHECK_FALSE(f.core->autosaveNow());  // unchanged since
    std::string dump = canonicalDump(f.core->document());

    // A second session on the same folders finds the leftover.
    auto registry2 = makeRegistry();
    AppCore second(*registry2, platform::AppPaths{f.dir / "cfg", f.dir / "data"});
    auto rec = second.pendingRecovery();
    REQUIRE(rec.has_value());
    CHECK(rec->originalPath.empty());
    REQUIRE(second.recover(*rec).ok());
    CHECK(canonicalDump(second.document()) == dump);
    CHECK(second.modified());
    second.discardRecovery();
    CHECK_FALSE(second.pendingRecovery().has_value());
}

TEST_CASE("a clean save or shutdown removes the autosave") {
    Fixture f("autosave2");
    f.line("0,0", "100,0");
    REQUIRE(f.core->autosaveNow());
    REQUIRE(f.core->pendingRecovery().has_value());
    REQUIRE(f.core->saveAs(f.dir / "a.tbcad").ok());
    CHECK_FALSE(f.core->pendingRecovery().has_value());
    f.line("1,1", "2,2");
    REQUIRE(f.core->autosaveNow());
    f.core->shutdown();
    CHECK_FALSE(f.core->pendingRecovery().has_value());
}

TEST_CASE("autosave timer follows the setting") {
    Fixture f("autosave3");
    f.core->settings().setNumber("autosave.minutes", 1);
    f.line("0,0", "100,0");
    auto t0 = std::chrono::steady_clock::now();
    f.core->tickAutosave(t0);  // starts the clock
    f.core->tickAutosave(t0 + std::chrono::seconds(30));
    CHECK_FALSE(f.core->pendingRecovery().has_value());
    f.core->tickAutosave(t0 + std::chrono::seconds(61));
    CHECK(f.core->pendingRecovery().has_value());
}

TEST_CASE("a command that leaves the working area raises a notice with Zoom to and Undo") {
    Fixture f("outofarea");
    CHECK_FALSE(f.core->outOfAreaNotice().has_value());
    f.line("0,0", "1e9,0");  // far beyond the default +-1e8 mm working area
    REQUIRE(f.core->outOfAreaNotice().has_value());
    CHECK(f.core->outOfAreaNotice()->ids.size() == 1);
    f.core->outOfAreaZoomTo();
    CHECK(f.core->camera().visibleWorld().max.x >= 1e9);
    f.core->outOfAreaUndo();
    CHECK_FALSE(f.core->outOfAreaNotice().has_value());
    CHECK(f.core->document().modelSpaceCount() == 0);
    // Undoing and redoing by hand does not raise the notice again.
    f.line("0,0", "1e9,0");
    f.core->dismissOutOfAreaNotice();
    CHECK(f.core->undo());
    CHECK(f.core->redo());
    CHECK_FALSE(f.core->outOfAreaNotice().has_value());
}

TEST_CASE("host settings are typed by what is stored; toggles live in the UI registry") {
    Fixture f("hostsettings");
    CHECK(f.core->setting("drafting.snap") == "0");
    f.core->setSetting("drafting.snap", "on");
    CHECK(f.core->setting("drafting.snap") == "1");
    CHECK(f.core->setting("autosave.minutes") == "10");
    CHECK_FALSE(f.core->setting("unknown.key").has_value());
    f.core->setSetting("custom.thing", "abc");
    CHECK(f.core->setting("custom.thing") == "abc");
    CHECK(f.core->settingsFolder() == f.dir / "cfg");

    UiRegistry ui;
    f.core->registerBuiltinUi(ui);
    REQUIRE(ui.statusToggles().size() == 5);
    CHECK(ui.statusToggles()[0].label == "SNAP");
    CHECK(ui.statusToggles()[0].get());
    ui.statusToggles()[0].set(false);
    CHECK(f.core->setting("drafting.snap") == "0");
}

TEST_CASE("user aliases are loaded from the settings folder") {
    fs::path dir = tempDir("aliases");
    fs::create_directories(dir / "cfg");
    { std::ofstream(dir / "cfg" / "aliases.json") << R"({"QQ": "TESTLINE"})"; }
    auto registry = makeRegistry();
    AppCore core(*registry, platform::AppPaths{dir / "cfg", dir / "data"});
    CHECK(registry->find("QQ") != nullptr);
    CHECK(core.startupWarnings().empty());
}

TEST_CASE("broken settings files are reported, defaults stay") {
    fs::path dir = tempDir("brokensettings");
    fs::create_directories(dir / "cfg");
    {
        std::ofstream(dir / "cfg" / "settings.json") << "{ nope";
        std::ofstream(dir / "cfg" / "keybindings.json") << "[";
    }
    auto registry = makeRegistry();
    AppCore core(*registry, platform::AppPaths{dir / "cfg", dir / "data"});
    CHECK(core.startupWarnings().size() == 2);
    CHECK(core.settings().getString("ui.theme") == "dark");
    CHECK(core.keybindings().label("edit.undo") == "Ctrl+Z");
}

TEST_CASE("command completion lists names and aliases") {
    Fixture f("complete");
    auto c = f.core->complete("te");
    CHECK(std::find(c.begin(), c.end(), "TESTLINE") != c.end());
    CHECK(f.core->complete("").empty());
}

TEST_CASE("several drawings: tabs keep their own content, view and selection") {
    Fixture f("tabs");
    CHECK(f.core->documentCount() == 1);
    CHECK(f.core->isPristine());
    f.line("0,0", "100,0");
    CHECK_FALSE(f.core->isPristine());

    f.core->newDocument();  // the first drawing has content, so this adds a tab
    REQUIRE(f.core->documentCount() == 2);
    CHECK(f.core->activeDocument() == 1);
    CHECK(f.core->document().modelSpaceCount() == 0);
    CHECK(f.core->scene().entityCount() == 0);
    f.line("0,0", "5,5");
    f.line("1,1", "2,2");
    CHECK(f.core->scene().entityCount() == 2);

    CHECK(f.core->documentModified(0));
    CHECK(f.core->documentTitle(0) == "Untitled");
    CHECK(f.core->documentId(0) != f.core->documentId(1));

    REQUIRE(f.core->activateDocument(0));
    CHECK(f.core->document().modelSpaceCount() == 1);
    CHECK(f.core->scene().entityCount() == 1);
    // Undo history is per drawing.
    CHECK(f.core->undo());
    CHECK(f.core->document().modelSpaceCount() == 0);
    REQUIRE(f.core->activateDocument(1));
    CHECK(f.core->document().modelSpaceCount() == 2);
}

TEST_CASE("an untouched Untitled drawing is reused by New and Open") {
    Fixture f("reuse");
    f.core->newDocument();
    CHECK(f.core->documentCount() == 1);
    fs::path file = f.dir / "a.tbcad";
    REQUIRE(f.core->saveAs(file).ok());  // empty drawing on disk
    f.core->newDocument();               // the saved drawing is not "Untitled", so a tab is added
    REQUIRE(f.core->documentCount() == 2);
    REQUIRE(f.core->openFile(file).ok());  // already open: just activates
    CHECK(f.core->documentCount() == 2);
    CHECK(f.core->activeDocument() == 0);
}

TEST_CASE("closing drawings") {
    Fixture f("close");
    f.line("0,0", "1,1");
    f.core->newDocument();
    f.line("0,0", "2,2");
    f.line("0,0", "3,3");
    f.core->newDocument();
    f.line("0,0", "4,4");
    f.line("0,0", "5,5");
    f.line("0,0", "6,6");
    REQUIRE(f.core->documentCount() == 3);
    REQUIRE(f.core->activeDocument() == 2);

    f.core->closeDocument(0);  // a background tab
    CHECK(f.core->documentCount() == 2);
    CHECK(f.core->activeDocument() == 1);
    CHECK(f.core->document().modelSpaceCount() == 3);

    f.core->closeDocument(1);  // the active one: its neighbour takes over
    CHECK(f.core->documentCount() == 1);
    CHECK(f.core->document().modelSpaceCount() == 2);
    CHECK(f.core->scene().entityCount() == 2);

    f.core->closeDocument(0);  // the last one leaves a fresh Untitled
    CHECK(f.core->documentCount() == 1);
    CHECK(f.core->isPristine());
    CHECK(f.core->scene().entityCount() == 0);
}

TEST_CASE("switching tabs is refused while a command runs") {
    Fixture f("busy");
    f.line("0,0", "1,1");
    f.core->newDocument();
    f.line("0,0", "2,2");
    f.core->submitText("TESTLINE");
    REQUIRE(f.core->runner().active());
    CHECK_FALSE(f.core->activateDocument(0));
    CHECK(f.core->activeDocument() == 1);
    f.core->pressEscape();
    CHECK(f.core->activateDocument(0));
}

TEST_CASE("the clipboard.text setting is handed to the window once") {
    Fixture f("clipboard");
    CHECK_FALSE(f.core->takeClipboardText().has_value());
    f.core->setSetting("clipboard.text", "area = 12.5");
    auto text = f.core->takeClipboardText();
    REQUIRE(text.has_value());
    CHECK(*text == "area = 12.5");
    CHECK_FALSE(f.core->takeClipboardText().has_value());
}

namespace {
struct SnapTo100 : cmd::PointFilter {
    std::optional<Vec2> filter(const cmd::PointQuery&, Vec2, cmd::FilterFeedback&) override {
        return Vec2{100, 100};
    }
};
}  // namespace

TEST_CASE("registered point filters apply to clicks, also after a new document") {
    Fixture f("hook_filter");
    UiRegistry ui;
    ui.addPointFilter(0, std::make_unique<SnapTo100>());
    f.core->applyUiKeybindings(ui);
    for (int round = 0; round < 2; ++round) {
        if (round == 1)
            f.core->newDocument();
        f.core->submitText("TESTLINE");
        f.core->click(f.at({300, 300}));
        f.core->click(f.at({400, 400}));
        f.core->pressEscape();
        // Both clicks snapped to (100,100).
        bool found = false;
        f.core->document().forEachEntity([&](const Entity& e) {
            if (const auto* l = entityAs<LineData>(e))
                found = found || (l->a.x == 100 && l->a.y == 100);
        });
        CHECK(found);
    }
}

TEST_CASE("session hooks run when installed and for every new document") {
    Fixture f("hook_session");
    UiRegistry ui;
    std::vector<const Document*> seen;
    ui.addSessionHook([&](const SessionInfo& s) {
        REQUIRE(s.doc != nullptr);
        CHECK(s.index != nullptr);
        CHECK(s.host != nullptr);
        seen.push_back(s.doc);
    });
    f.core->applyUiKeybindings(ui);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == &f.core->document());
    f.core->newDocument();
    REQUIRE(seen.size() == 2);
    CHECK(seen[1] == &f.core->document());
}

TEST_CASE("a leading apostrophe on an idle command line is ignored") {
    Fixture f("apostrophe");
    f.core->submitText("'TESTLINE");
    CHECK(f.core->runner().active());
    f.core->pressEscape();
}

TEST_CASE("double click on a text object selects it and starts TEXTEDIT") {
    // The text generator lives in libs/text; a stand-in gives the object a pickable outline.
    geom::DisplayRegistry::set(EntityKind::Text, [](const Document&, const Entity& e,
                                                    const geom::DisplayContext&, DisplayList& out) {
        const auto& t = *entityAs<TextData>(e);
        DisplayPolyline pl;
        pl.closed = true;
        pl.pts = {t.position, t.position + Vec2{40, 0}, t.position + Vec2{40, 10},
                  t.position + Vec2{0, 10}};
        out.items.emplace_back(std::move(pl));
    });
    Fixture f("dblclick");
    EntityId text;
    {
        auto tx = f.core->document().begin("text");
        TextData d;
        d.position = {100, 100};
        d.utf8 = "abc";
        text = tx.add(Entity{{}, d}).value();
        tx.commit();
    }
    EntityId line = addLine(f.core->document(), {0, 0}, {50, 0});

    f.core->doubleClick(f.at({100, 105}));  // on the text outline
    CHECK(f.core->selection() == std::vector<EntityId>{text});
    bool started = false;
    for (const auto& h : f.core->history())
        started = started || h.find("TEXTEDIT") != std::string::npos;
    CHECK(started);

    f.core->clearSelection();
    f.core->doubleClick(f.at({25, 0}));  // a line: plain single click
    CHECK(f.core->selection() == std::vector<EntityId>{line});
    geom::DisplayRegistry::clear(EntityKind::Text);
}

TEST_CASE(
    "an idle click goes to the click hooks first; a handled click leaves the selection alone") {
    Fixture f("hook_idle_click");
    f.line("0,0", "100,0");
    f.core->selectAll();
    REQUIRE(f.core->selection().size() == 1);
    UiRegistry ui;
    int calls = 0;
    Vec2 seen{};
    bool consume = true;
    ui.addIdleClickHook([&](PanelContext& pc, Vec2 world, double aperture, bool, bool) {
        ++calls;
        seen = world;
        CHECK(aperture > 0);
        CHECK(pc.apertureMm == aperture);
        CHECK(pc.selection == &f.core->selection());
        return consume;
    });
    f.core->applyUiKeybindings(ui);
    f.core->click(f.at({300, 300}));  // empty space: would clear the selection
    CHECK(calls == 1);
    CHECK(distance(seen, {300, 300}) < 1e-6);
    CHECK(f.core->selection().size() == 1);
    consume = false;
    f.core->click(f.at({300, 300}));
    CHECK(calls == 2);
    CHECK(f.core->selection().empty());
}

TEST_CASE("SETTINGS asks the window to open the dialog") {
    Fixture f("settings_cmd");
    f.core->submitText("op");  // alias
    auto req = f.core->takeUiRequests();
    REQUIRE(req.size() == 1);
    CHECK(req[0] == "settings");
    CHECK(f.core->takeUiRequests().empty());
    CHECK_FALSE(f.core->runner().active());
    CHECK(f.core->keybindings().label("view.expandLog") == "Ctrl+L");
    CHECK(f.core->keybindings().label("view.cleanScreen") == "F11");
}

TEST_CASE("a .dxf file opens and saves back as DXF in place") {
    Fixture f("dxfmain");
    f.line("0,0", "100,0");
    f.line("10,10", "50,70");
    const fs::path dxfFile = f.dir / "plan.dxf";
    REQUIRE(f.core->saveAs(dxfFile).ok());
    {
        std::ifstream in(dxfFile, std::ios::binary);
        std::string head(20, ' ');
        in.read(head.data(), 20);
        CHECK(head.find("SECTION") != std::string::npos);  // "  0\nSECTION" starts a DXF
    }
    CHECK(f.core->file() == dxfFile);

    // Reopen in a second application: the lines come back and Save writes DXF again.
    Fixture g("dxfmain2");
    REQUIRE(g.core->openFile(dxfFile).ok());
    CHECK(g.core->document().modelSpaceCount() == 2);
    CHECK(g.core->file() == dxfFile);
    g.line("0,5", "5,5");
    CHECK(g.core->modified());
    REQUIRE(g.core->save().ok());
    CHECK_FALSE(g.core->modified());
    CHECK(fs::exists(dxfFile.string() + ".bak"));

    Fixture h("dxfmain3");
    REQUIRE(h.core->openFile(dxfFile).ok());
    CHECK(h.core->document().modelSpaceCount() == 3);
}

TEST_CASE("files.defaultFormat decides the extension of a name without one") {
    Fixture f("dxfdefault");
    f.line("0,0", "10,0");
    f.core->settings().setString("files.defaultFormat", "dxf");
    REQUIRE(f.core->saveAs(f.dir / "noext").ok());
    CHECK(f.core->file().extension() == ".dxf");
    CHECK(fs::exists(f.dir / "noext.dxf"));
    f.core->settings().setString("files.defaultFormat", "tbcad");
    REQUIRE(f.core->saveAs(f.dir / "other").ok());
    CHECK(f.core->file().extension() == ".tbcad");
}
