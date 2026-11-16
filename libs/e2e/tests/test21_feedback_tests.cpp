// Bugs from the owner's test.21 hand test, replayed by typing into the real application core
// (the same text the owner typed on the command line).
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "tbcad/cmd/register_all.h"
#include "tbcad/layout/layout.h"
#include "tbcad/ui/app_core.h"

#ifdef _WIN32
#include <process.h>
#define TBCAD_GETPID _getpid
#else
#include <unistd.h>
#define TBCAD_GETPID getpid
#endif

using namespace tbcad;

namespace {

struct App {
    std::filesystem::path dir;
    cmd::Registry registry;
    std::unique_ptr<ui::AppCore> core;
    explicit App(const char* name) {
        dir = std::filesystem::temp_directory_path() /
              (std::string("tbcad_t21_") + name + "_" + std::to_string(TBCAD_GETPID()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        registerAllCommands(registry);
        core =
            std::make_unique<ui::AppCore>(registry, platform::AppPaths{dir / "cfg", dir / "data"});
        core->setViewportSize(1400, 900);
    }
    ~App() {
        core.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    void type(std::initializer_list<const char*> lines) {
        for (const char* l : lines)
            core->submitText(l);
    }
    bool said(std::string_view text) const {
        for (const std::string& h : core->history())
            if (h.find(text) != std::string::npos)
                return true;
        return false;
    }
    std::size_t count(EntityKind k, BlockId owner = {}) const {
        std::size_t n = 0;
        const Document& doc = core->document();
        doc.forEachEntity([&](const Entity& e) {
            n += kind(e) == k && doc.ownerBlock(e.common.id) == owner ? 1 : 0;
        });
        return n;
    }
};

}  // namespace

TEST_CASE("test.21: BREAK picks an MLINE wall at a typed point") {
    for (const char* first : {"3000,-6000", "3000,-5950"}) {
        App a("break");
        a.type({"MLINE", "S", "100", "0,-6000", "6000,-6000", ""});
        REQUIRE(a.count(EntityKind::MLine) == 1);
        a.core->zoomExtents();
        a.type({"BREAK", first, "4000,-6000"});
        CHECK_FALSE(a.said("No object at"));
        CHECK(a.count(EntityKind::MLine) == 2);
    }
}

TEST_CASE("test.21: -BLOCK with Last takes a rectangle drawn on a layout sheet") {
    App a("block");
    a.type({"LAYOUT", "New", "Sheet", "A3", ""});
    const auto lay = layout::findLayout(a.core->document(), "Sheet");
    REQUIRE(lay);
    REQUIRE(a.core->drawingOwner() == lay->content);
    a.type({"RECTANG", "10,10", "60,40"});
    REQUIRE(a.count(EntityKind::Polyline, lay->content) == 1);
    a.type({"-BLOCK", "Stamp", "Last", "", "10,10", "C"});
    CHECK_FALSE(a.said("No objects found"));
    const BlockDef* def = a.core->document().blockByName("Stamp");
    REQUIRE(def);
    CHECK(a.count(EntityKind::Polyline, def->id) == 1);
    CHECK(a.count(EntityKind::Polyline, lay->content) == 0);
    CHECK(a.count(EntityKind::Insert, lay->content) == 1);  // the block stays on the sheet
}
