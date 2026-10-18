#include <filesystem>
#include <fstream>
#include <sstream>

#include "support.h"
#include "tbcad/table/table.h"

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

TEST_CASE("ATTTABLE makes a schedule table with the block name and a column per tag") {
    Fixture f;
    EntityId ln = f.addLine(0, 0, 10, 0);
    REQUIRE(f.host->run("ATTDEF", {"", "name", "Name", "Anon", "0,3", "2", "0"}).finished);
    std::vector<EntityId> ids{ln};
    for (EntityId id : f.modelSpace())
        if (id != ln)
            ids.push_back(id);
    f.host->setSelection(ids);
    f.host->run("BLOCK", {"Tag", "0,0", "D"});
    f.host->run("INSERT", {"Tag", "100,50", "Room A", cmd::ScriptItem::enter()});
    f.host->run("INSERT", {"Tag", "200,50", "Hall", cmd::ScriptItem::enter()});
    auto r = f.host->run("ATTTABLE", {cmd::ScriptItem::enter(), "0,-100"});
    CHECK(r.finished);
    const TableData* t = nullptr;
    for (EntityId id : f.modelSpace())
        if (const auto* td = entityAs<TableData>(*f.doc.find(id)))
            t = td;
    REQUIRE(t);
    CHECK(table::rowCount(*t) == 3);  // header + two inserts
    CHECK(table::colCount(*t) == 2);  // Block, NAME
    CHECK(table::cellAt(*t, 0, 1).utf8 == "NAME");
    CHECK(table::cellAt(*t, 1, 1).utf8 == "Room A");
    CHECK(table::cellAt(*t, 2, 0).utf8 == "Tag");
}

TEST_CASE("ATTIN puts ATTEXT values back, including commas, quotes and Thai") {
    Fixture f;
    EntityId ln = f.addLine(0, 0, 10, 0);
    REQUIRE(f.host->run("ATTDEF", {"", "name", "Name", "Anon", "0,3", "2", "0"}).finished);
    std::vector<EntityId> ids{ln};
    for (EntityId id : f.modelSpace())
        if (id != ln)
            ids.push_back(id);
    f.host->setSelection(ids);
    f.host->run("BLOCK", {"Tag", "0,0", "D"});
    f.host->run("INSERT", {"Tag", "100,50", "A", cmd::ScriptItem::enter()});
    f.host->run("INSERT", {"Tag", "100,50", "B", cmd::ScriptItem::enter()});
    f.host->run("INSERT", {"Tag", "200,50", "C", cmd::ScriptItem::enter()});

    const auto dir = std::filesystem::temp_directory_path() / "tbcad_attin";
    std::filesystem::create_directories(dir);
    const auto file = dir / "in.csv";
    f.host->run("ATTOUT", {cmd::ScriptItem::enter(), file.string()});
    // Edit the file as a person would in Excel.
    std::string csv;
    {
        std::ifstream in(file, std::ios::binary);
        std::stringstream b;
        b << in.rdbuf();
        csv = b.str();
    }
    auto replace = [&](const std::string& a, const std::string& b) {
        auto p = csv.find(a);
        REQUIRE(p != std::string::npos);
        csv.replace(p, a.size(), b);
    };
    replace(",A\r\n", ",\"Room, \"\"1\"\"\"\r\n");
    replace(",C\r\n", ",\u0E2B\u0E49\u0E2D\u0E07\r\n");
    {
        std::ofstream o(file, std::ios::binary);
        o << csv;
    }
    auto values = [&] {
        std::vector<std::string> v;
        for (EntityId id : f.modelSpace())
            if (const auto* ins = entityAs<InsertData>(*f.doc.find(id)))
                v.push_back(ins->attribs.at(0).value);
        return v;
    };
    f.host->run("ATTIN", {file.string()});
    CHECK(f.host->logContains("2 inserts updated"));
    CHECK(values() == std::vector<std::string>{"Room, \"1\"", "B", "\u0E2B\u0E49\u0E2D\u0E07"});
    CHECK(f.host->undo());
    CHECK(values() == std::vector<std::string>{"A", "B", "C"});
    f.host->run("ATTIN", {(dir / "missing.csv").string()});
    CHECK(f.host->logContains("Cannot read"));
}
