// The title asterisk and the close prompt follow the modified flag: every kind of edit sets it,
// Save clears it, and pure view changes (zoom, pan, selection, layout tab switch) leave it alone.
#include <doctest/doctest.h>

#include <filesystem>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "tbcad/cmd/register_all.h"
#include "tbcad/ui/app_core.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;
namespace fs = std::filesystem;

namespace {

struct Session {
    fs::path dir;
    cmd::Registry registry;
    ui::UiRegistry uiRegistry;
    std::unique_ptr<ui::AppCore> core;
    fs::path file;

    Session() {
        dir = fs::temp_directory_path() / ("tbcad_dirty_flag_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        registerAllUi(uiRegistry);
        core->registerBuiltinUi(uiRegistry);
        core->setViewportSize(1000, 800);
        file = dir / "work.tbcad";
        core->submitText("LINE 0,0 100,50 ");
        REQUIRE(core->saveAs(file).ok());
        REQUIRE_FALSE(core->modified());
    }
    ~Session() {
        core.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // Runs `edit`, expects the drawing to be modified with a starred title and the tab marked,
    // then saves and expects everything clean again.
    template <class F>
    void editThenSave(const char* what, F&& edit) {
        INFO(what);
        edit();
        CHECK(core->modified());
        CHECK(core->anyDocumentModified());
        CHECK(core->windowTitle().find("work.tbcad*") != std::string::npos);
        REQUIRE(core->save().ok());
        CHECK_FALSE(core->modified());
        CHECK_FALSE(core->anyDocumentModified());
        CHECK(core->windowTitle().find('*') == std::string::npos);
    }
};

}  // namespace

TEST_CASE("every kind of edit marks the drawing modified and Save clears it") {
    Session s;
    s.editThenSave("geometry by command", [&] { s.core->submitText("CIRCLE 50,50 30"); });
    s.editThenSave("layer row", [&] {
        auto tx = s.core->document().begin("layer");
        Layer l;
        l.name = "DirtyTestLayer";
        auto r = tx.addLayer(l);
        if (!r.ok())
            MESSAGE(r.error().message);
        REQUIRE(r.ok());
        tx.commit();
    });
    s.editThenSave("text style row", [&] {
        auto tx = s.core->document().begin("style");
        TextStyle t;
        t.name = "DirtyTestStyle";
        REQUIRE(tx.addTextStyle(t).ok());
        tx.commit();
    });
    s.editThenSave("system variable saved in the drawing",
                   [&] { s.core->submitText("LTSCALE 3"); });
    s.editThenSave("block definition", [&] {
        auto tx = s.core->document().begin("block");
        BlockDef b;
        b.name = "Mark";
        REQUIRE(tx.addBlock(b).ok());
        tx.commit();
    });
    s.editThenSave("new layout", [&] { s.core->submitText("LAYOUT New Sheet1 A3 Landscape"); });
}

TEST_CASE("view changes and selection do not mark the drawing modified") {
    Session s;
    s.core->submitText("ZOOM E");
    s.core->submitText("-PAN 0,0 20,20");
    render::Camera2D cam = s.core->camera();
    cam.pixelsPerMm *= 2;
    s.core->setCamera(cam);
    s.core->selectAll();
    s.core->clearSelection();
    CHECK_FALSE(s.core->modified());

    s.editThenSave("layout for the tab switch",
                   [&] { s.core->submitText("LAYOUT New Sheet1 A3 Landscape"); });
    REQUIRE(s.core->setActiveLayout("Sheet1"));
    CHECK_FALSE(s.core->modified());
    REQUIRE(s.core->setActiveLayout(""));
    CHECK_FALSE(s.core->modified());
    CHECK(s.core->windowTitle().find('*') == std::string::npos);
}

TEST_CASE("reopening a saved drawing starts clean") {
    Session s;
    s.core->submitText("CIRCLE 50,50 30");
    REQUIRE(s.core->save().ok());
    s.core->newDocument();
    REQUIRE(s.core->openFile(s.file).ok());
    CHECK_FALSE(s.core->modified());
}
