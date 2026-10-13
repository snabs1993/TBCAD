#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>

#include "helpers.h"
#include "tbcad/core/dump.h"

using namespace tbcad;
using namespace tbcad::testing;
namespace fs = std::filesystem;

namespace {
std::string dumpOf(const fs::path& p) {
    io::IoReport report;
    auto r = io::readTbcad(p, report);
    TB_REQUIRE_OK(r);
    return canonicalDump(*r.value());
}
std::unique_ptr<Document> docWith(int lines) {
    auto doc = std::make_unique<Document>();
    Transaction tx = doc->begin("t");
    for (int i = 0; i < lines; ++i) {
        Entity e;
        e.common.layer = kLayerZero;
        e.data = LineData{{double(i), 0}, {double(i), 1}};
        (void)tx.add(std::move(e));
    }
    tx.commit();
    return doc;
}
}  // namespace

TEST_CASE("save: new file, no temporary left behind") {
    TempDir dir;
    fs::path file = dir.path() / "a.tbcad";
    auto doc = makeRichDocument();
    REQUIRE(io::writeTbcad(*doc, file).ok());
    CHECK(fs::exists(file));
    CHECK_FALSE(fs::exists(dir.path() / "a.tbcad.tmp"));
    CHECK_FALSE(fs::exists(dir.path() / "a.tbcad.bak"));
    CHECK(dumpOf(file) == canonicalDump(*doc));
}

TEST_CASE("save: the previous file is kept as .bak") {
    TempDir dir;
    fs::path file = dir.path() / "a.tbcad";
    auto one = docWith(1);
    auto two = docWith(2);
    REQUIRE(io::writeTbcad(*one, file).ok());
    REQUIRE(io::writeTbcad(*two, file).ok());
    CHECK(dumpOf(file) == canonicalDump(*two));
    CHECK(dumpOf(dir.path() / "a.tbcad.bak") == canonicalDump(*one));
    CHECK_FALSE(fs::exists(dir.path() / "a.tbcad.tmp"));

    io::WriteOptions noBackup;
    noBackup.keepBackup = false;
    TempDir dir2;
    fs::path f2 = dir2.path() / "b.tbcad";
    REQUIRE(io::writeTbcad(*one, f2, noBackup).ok());
    REQUIRE(io::writeTbcad(*two, f2, noBackup).ok());
    CHECK_FALSE(fs::exists(dir2.path() / "b.tbcad.bak"));
    CHECK(dumpOf(f2) == canonicalDump(*two));
}

TEST_CASE("save: a crash between the temporary write and the replace leaves the original intact") {
    TempDir dir;
    fs::path file = dir.path() / "a.tbcad";
    auto one = docWith(1);
    auto two = docWith(2);
    REQUIRE(io::writeTbcad(*one, file).ok());

    io::WriteOptions crash;
    crash.simulateCrashBeforeReplace = true;
    CHECK_FALSE(io::writeTbcad(*two, file, crash).ok());

    CHECK(dumpOf(file) == canonicalDump(*one));  // the original still opens, unchanged
    CHECK(fs::exists(dir.path() / "a.tbcad.tmp"));
    CHECK(dumpOf(dir.path() / "a.tbcad.tmp") == canonicalDump(*two));  // the tmp is complete

    // The next real save cleans up and wins.
    REQUIRE(io::writeTbcad(*two, file).ok());
    CHECK(dumpOf(file) == canonicalDump(*two));
    CHECK_FALSE(fs::exists(dir.path() / "a.tbcad.tmp"));
}

TEST_CASE("save: errors are reported, not thrown") {
    TempDir dir;
    auto doc = docWith(1);
    CHECK_FALSE(io::writeTbcad(*doc, dir.path() / "no-such-dir" / "a.tbcad").ok());
    io::IoReport report;
    auto r = io::readTbcad(dir.path() / "missing.tbcad", report);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == ErrorCode::Io);
}

TEST_CASE("a file with a damaged tail does not replace a good backup silently") {
    TempDir dir;
    fs::path file = dir.path() / "a.tbcad";
    auto doc = docWith(3);
    REQUIRE(io::writeTbcad(*doc, file).ok());
    // Truncate the file on disk: reading must fail with a message, not crash.
    fs::resize_file(file, fs::file_size(file) - 7);
    io::IoReport report;
    auto r = io::readTbcad(file, report);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("truncated") != std::string::npos);
}

#ifdef TBCAD_DUMP_EXE
TEST_CASE("tbcad-dump prints the chunk list and the canonical dump") {
    TempDir dir;
    fs::path file = dir.path() / "a.tbcad";
    fs::path out = dir.path() / "dump.txt";
    auto doc = makeRichDocument();
    REQUIRE(io::writeTbcad(*doc, file).ok());
    std::string cmd = std::string("\"") + TBCAD_DUMP_EXE + "\" \"" + file.string() + "\" > \"" +
                      out.string() + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::ifstream in(out, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("chunk HEAD") != std::string::npos);
    CHECK(text.find("chunk END") != std::string::npos);
    CHECK(text.find(canonicalDump(*doc)) != std::string::npos);

    // A damaged file exits with 1.
    fs::resize_file(file, 30);
    CHECK(std::system(cmd.c_str()) != 0);
}
#endif
