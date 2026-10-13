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
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Built-in");
    CHECK(names[1] == "Plan A");

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
