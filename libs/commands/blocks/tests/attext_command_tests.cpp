#include <filesystem>
#include <fstream>
#include <sstream>

#include "support.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;

TEST_CASE("ATTEXT writes one CSV row per insert with a column per attribute tag") {
    Fixture f;
    EntityId ln = f.addLine(0, 0, 10, 0);
    REQUIRE(f.host->run("ATTDEF", {"", "name", "Name", "Anon", "0,3", "2", "0"}).finished);
    REQUIRE(f.host->run("ATTDEF", {"C", "", "kind", "Door", "0,6", "", ""}).finished);
    std::vector<EntityId> ids{ln};
    for (EntityId id : f.modelSpace())
        if (id != ln)
            ids.push_back(id);
    f.host->setSelection(ids);
    f.host->run("BLOCK", {"Tag", "0,0", "D"});
    REQUIRE(f.doc.blockByName("Tag"));
    f.host->run("INSERT", {"Tag", "100,50", "Room, A", cmd::ScriptItem::enter()});
    f.host->run("INSERT", {"Tag", "200,50", "Hall", cmd::ScriptItem::enter()});

    const auto dir = std::filesystem::temp_directory_path() / "tbcad_attext";
    std::filesystem::create_directories(dir);
    const auto file = dir / "out.csv";
    std::filesystem::remove(file);
    auto r = f.host->run("ATTEXT", {cmd::ScriptItem::enter(), file.string()});
    CHECK(r.finished);
    std::ifstream in(file, std::ios::binary);
    REQUIRE(in);
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string csv = buf.str();
    CHECK(csv.rfind("\xEF\xBB\xBF"
                    "Block,Layer,X,Y,Rotation,ScaleX,ScaleY,NAME,KIND",
                    0) == 0);
    CHECK(csv.find("Tag,0,100,50,0,1,1,\"Room, A\",Door") != std::string::npos);
    CHECK(csv.find("Tag,0,200,50,0,1,1,Hall,Door") != std::string::npos);
}

TEST_CASE("ATTEXT says so when nothing has attributes") {
    Fixture f;
    f.addLine(0, 0, 10, 0);
    f.host->run("ATTEXT", {cmd::ScriptItem::enter()});
    CHECK(f.host->logContains("No inserts with attributes"));
}
