#include <filesystem>

#include "support.h"
#include "tbcad/io/tbcad.h"

using namespace tbcad;
using namespace tbcad::block_cmds;
using namespace tbcad::block_cmds::test;
namespace fs = std::filesystem;

namespace {

struct Dir {
    fs::path path;
    Dir() {
        path = fs::temp_directory_path() / "tbcad_xref_cmds";
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Dir() { fs::remove_all(path); }
};

fs::path writeSource(const fs::path& dir, const char* name, int lines) {
    Document d;
    {
        Transaction tx = d.begin("source");
        Layer l;
        l.name = "walls";
        LayerId w = tx.addLayer(l).value();
        for (int i = 0; i < lines; ++i) {
            Entity e;
            e.data = LineData{{double(i), 0}, {double(i), 10}};
            e.common.layer = w;
            (void)tx.add(e);
        }
        tx.commit();
    }
    fs::path file = dir / name;
    REQUIRE(io::writeTbcad(d, file, io::WriteOptions{.keepBackup = false}).ok());
    return file;
}

}  // namespace

TEST_CASE("XATTACH attaches a file and inserts it; undo removes everything") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 3);
    Fixture f;
    auto r = f.host->run("XATTACH", {file.string(), "", "", "", "10,20"});
    CHECK(r.finished);
    const BlockDef* def = f.doc.blockByName("plan");
    REQUIRE(def);
    CHECK(fs::path(def->xrefPath) == file);
    CHECK(def->entities.size() == 3);
    REQUIRE(f.modelSpace().size() == 1);
    const auto* ins = entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]));
    REQUIRE(ins);
    CHECK(ins->position == Vec2{10, 20});
    CHECK(f.doc.layerByName("plan") != nullptr);
    CHECK(f.host->undoStack().undoLabel() == "Attach plan");
    CHECK(f.host->undo());
    CHECK(f.doc.blockByName("plan") == nullptr);
    CHECK(f.doc.layerByName("plan") == nullptr);
    CHECK(f.modelSpace().empty());
}

TEST_CASE("XATTACH rejects a missing file and asks again; Enter inserts at the origin") {
    Dir dir;
    fs::path file = writeSource(dir.path, "a.tbcad", 1);
    Fixture f;
    f.host->clearLog();
    f.host->run("XATTACH", {(dir.path / "nope.tbcad").string(), file.string(), "", "", "site", ""});
    CHECK(f.host->logContains("Cannot find"));
    REQUIRE(f.modelSpace().size() == 1);
    CHECK(entityAs<InsertData>(*f.doc.find(f.modelSpace()[0]))->position == Vec2{0, 0});
    CHECK(f.doc.blockByName("site") != nullptr);
}

TEST_CASE("XRELOAD picks up a changed file; XLIST reports the state") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 2);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "", "", "0,0"});
    REQUIRE(f.doc.blockByName("plan"));
    CHECK(f.doc.blockByName("plan")->entities.size() == 2);

    writeSource(dir.path, "plan.tbcad", 5);
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
    f.host->clearLog();
    f.host->run("XLIST", {});
    CHECK(f.host->logContains("changed on disk"));
    f.host->run("XRELOAD", {""});
    CHECK(f.doc.blockByName("plan")->entities.size() == 5);
    CHECK(f.modelSpace().size() == 1);  // the insert stays
    f.host->clearLog();
    f.host->run("XLIST", {});
    CHECK(f.host->logContains("up to date"));
    CHECK(f.host->undo());
    CHECK(f.doc.blockByName("plan")->entities.size() == 2);
}

TEST_CASE("XBIND turns the xref into a block; XDETACH removes it") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 2);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "", "", "0,0"});
    f.host->run("XATTACH", {file.string(), "", "", "other", "5,5"});
    REQUIRE(f.doc.blockByName("other"));

    f.host->run("XBIND", {"plan"});
    CHECK(f.doc.blockByName("plan")->xrefPath.empty());
    f.host->clearLog();
    f.host->run("XBIND", {"plan", "other"});
    CHECK(f.host->logContains("No external reference named"));
    CHECK(f.doc.blockByName("other")->xrefPath.empty());

    f.host->run("XATTACH", {file.string(), "", "", "third", "1,1"});
    f.host->run("XDETACH", {"third"});
    CHECK(f.doc.blockByName("third") == nullptr);
    CHECK(f.doc.layerByName("third") == nullptr);
    CHECK(f.modelSpace().size() == 2);
}

TEST_CASE("XATTACH Overlay with an absolute path; XLIST says overlay") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 2);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "Overlay", "Absolute", "", "0,0"});
    const BlockDef* def = f.doc.blockByName("plan");
    REQUIRE(def);
    CHECK(def->xrefOverlay);
    CHECK(fs::path(def->xrefPath) == file);
    f.host->clearLog();
    f.host->run("XLIST", {});
    CHECK(f.host->logContains("overlay"));
}

TEST_CASE("XATTACH with the name-only path type stores just the file name") {
    Dir dir;
    fs::path file = writeSource(dir.path, "plan.tbcad", 1);
    Fixture f;
    f.host->run("XATTACH", {file.string(), "", "None", "", "0,0"});
    REQUIRE(f.doc.blockByName("plan"));
    CHECK(f.doc.blockByName("plan")->xrefPath == "plan.tbcad");
}
