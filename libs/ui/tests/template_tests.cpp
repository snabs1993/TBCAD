#include <doctest/doctest.h>

#include "tbcad/tmpl/tmpl.h"
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
    }
};
}  // namespace

TEST_CASE("NEW starts from the built-in template with the standard layers") {
    Fixture f("tmpl-builtin");
    f.core->newDocument();
    CHECK(f.core->document().layerByName("Dimensions"));
    CHECK(f.core->document().layerByName("Defpoints"));
    CHECK_FALSE(f.core->modified());
    CHECK(f.core->isPristine());
}

TEST_CASE("SAVEASTEMPLATE, TEMPLATEDEFAULT and NEWFROMTEMPLATE") {
    Fixture f("tmpl-commands");
    f.core->submitText("TESTLINE");
    f.core->submitText("0,0");
    f.core->submitText("10,0");
    f.core->submitText("SAVEASTEMPLATE");
    f.core->submitText("Plan A");
    CHECK(fs::exists(f.core->templateFolder() / "Plan A.tbt"));
    auto names = f.core->templateNames();
    REQUIRE(names.size() == 3);
    CHECK(names[0] == "Built-in");
    CHECK(names[1] == "Built-in (inch)");
    CHECK(names[2] == "Plan A");

    CHECK(f.core->newFromTemplate("Plan A").empty());
    CHECK(f.core->document().modelSpaceCount() == 1);
    CHECK(f.core->documentCount() == 2);
    CHECK_FALSE(f.core->hasFile());  // saving asks for a name

    CHECK_FALSE(f.core->newFromTemplate("Nope").empty());

    f.core->submitText("TEMPLATEDEFAULT");
    f.core->submitText("Plan A");
    CHECK(f.core->setting("template.default").value_or("") == "Plan A");
    f.core->newDocument();
    CHECK(f.core->document().modelSpaceCount() == 1);

    f.core->submitText("TEMPLATEDEFAULT");
    f.core->submitText("Nope");
    CHECK(f.core->history().back().find("No template") != std::string::npos);
    CHECK(f.core->setting("template.default").value_or("") == "Plan A");
    f.core->submitText("TEMPLATEDEFAULT");
    f.core->submitText("Built-in");
    CHECK(f.core->setting("template.default").value_or("x").empty());
}

TEST_CASE("a missing default template falls back to the built-in drawing") {
    Fixture f("tmpl-missing");
    f.core->setSetting("template.default", "Gone");
    f.core->newDocument();
    CHECK(f.core->document().layerByName("Walls"));
    bool said = false;
    for (const std::string& h : f.core->history())
        said = said || h.find("Gone") != std::string::npos;
    CHECK(said);
}

TEST_CASE("opening a .tbt gives an untitled copy") {
    Fixture f("tmpl-open");
    REQUIRE(f.core->saveAsTemplate("Copy").empty());
    const fs::path file = f.core->templateFolder() / "Copy.tbt";
    REQUIRE(f.core->openFile(file));
    CHECK_FALSE(f.core->hasFile());
    CHECK(f.core->document().layerByName("Walls"));
}

TEST_CASE("SAVE, QSAVE, SAVEAS, OPEN, NEW and CLOSE ask the window - SAVE is not SAVEASTEMPLATE") {
    Fixture f("file-commands");
    for (const char* line : {"SAVE", "qsave", "SAVEAS", "OPEN", "NEW", "CLOSE"})
        f.core->submitText(line);
    const std::vector<std::string> asked = f.core->takeUiRequests();
    const std::vector<std::string> expected = {"action:file.save",   "action:file.save",
                                               "action:file.saveAs", "action:file.open",
                                               "action:file.new",    "action:file.close"};
    CHECK(asked == expected);
    CHECK_FALSE(f.core->runner().active());  // nothing is waiting for a template name
}

TEST_CASE("saving a drawing asks the window for its picture for the start page") {
    Fixture f("tmpl-thumb");
    CHECK(f.core->pendingThumbnail().empty());
    const fs::path file = f.dir / "plan.tbcad";
    REQUIRE(f.core->saveAs(file).ok());
    for (int i = 0; i < 5 && f.core->sceneBuilding(); ++i)
        f.core->updateFrame();
    CHECK(f.core->pendingThumbnail() == platform::pathToUtf8(file));
    // One picture per drawing path, in the data folder.
    const fs::path png = f.core->thumbnailFile(platform::pathToUtf8(file));
    CHECK(png.parent_path() == f.dir / "data" / "thumbnails");
    CHECK(png.extension() == ".png");
    CHECK(png == f.core->thumbnailFile(platform::pathToUtf8(file)));
    CHECK(png != f.core->thumbnailFile(platform::pathToUtf8(f.dir / "other.tbcad")));
    f.core->thumbnailTaken();
    CHECK(f.core->pendingThumbnail().empty());
    // Another drawing in front: the picture waits for its own drawing.
    REQUIRE(f.core->saveAs(file).ok());
    f.core->newDocument();
    CHECK(f.core->pendingThumbnail().empty());
}

TEST_CASE("a .dxf in the template folder is a template, SAVEAS .tbt makes a template copy") {
    Fixture f("tmpl-dxf");
    f.core->submitText("TESTLINE");
    f.core->submitText("0,0");
    f.core->submitText("10,0");
    fs::create_directories(f.core->templateFolder());
    REQUIRE(f.core->saveAs(f.core->templateFolder() / "Sheet.dxf").ok());

    const auto names = f.core->templateNames();
    CHECK(std::find(names.begin(), names.end(), "Sheet.dxf") != names.end());
    const std::size_t before = f.core->documentCount();
    CHECK(f.core->newFromTemplate("Sheet.dxf").empty());
    CHECK(f.core->documentCount() == before + 1);
    CHECK_FALSE(f.core->hasFile());  // the template file is never overwritten by SAVE
    CHECK(f.core->document().modelSpaceCount() == 1);
    CHECK_FALSE(f.core->newFromTemplate("Missing.dxf").empty());

    // As the default, by full path.
    f.core->setSetting("template.default", "Sheet.dxf");
    f.core->newDocument();
    CHECK(f.core->document().modelSpaceCount() == 1);

    // Save As with the .tbt extension writes a template and leaves this drawing alone.
    f.core->newDocument();
    const fs::path tbt = f.dir / "Office.tbt";
    CHECK(f.core->saveAs(tbt).ok());
    CHECK(fs::exists(tbt));
    CHECK_FALSE(f.core->hasFile());
}
