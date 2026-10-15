#include <filesystem>

#include "support.h"
#include "tbcad/xref/xref.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;
namespace fs = std::filesystem;

TEST_CASE("every starter library symbol loads and has geometry") {
    const fs::path dir = fs::path(TBCAD_SOURCE_ASSETS_DIR) / "blocks";
    REQUIRE(fs::is_directory(dir));
    int files = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".dxf")
            continue;
        ++files;
        auto src = xref::loadSource(entry.path());
        INFO(entry.path().filename().string());
        REQUIRE(src.ok());
        CHECK(src.value().doc->modelSpaceCount() > 0);
    }
    CHECK(files >= 20);
}

TEST_CASE("a starter symbol can be inserted by name from the library folder") {
    Fixture f;
    f.host->setSetting("block.library",
                       (fs::path(TBCAD_SOURCE_ASSETS_DIR) / "blocks").generic_string());
    auto r = f.host->run("INSERT", {"Door-900", "100,50"});
    INFO(r.error);
    for (const auto& [id, def] : f.doc.blocks())
        INFO(def.name);
    REQUIRE(f.doc.blockByName("Door-900"));
    CHECK(f.doc.blockByName("Door-900")->entities.size() == 2);  // the leaf and the swing
    CHECK(f.modelSpace().size() == 1);
}
