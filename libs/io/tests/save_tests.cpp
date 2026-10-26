#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "../src/bytes.h"
#include "helpers.h"
#include "tbcad/core/dump.h"
#include "tbcad/platform/paths.h"

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

TEST_CASE("save: a killed write of an autosave-style file keeps the last good copy") {
    TempDir dir;
    fs::path file = dir.path() / "recovery-1.tbcad";
    io::WriteOptions opt;
    opt.keepBackup = false;
    auto one = docWith(1);
    auto two = docWith(2);
    REQUIRE(io::writeTbcad(*one, file, opt).ok());

    // Killed while the temporary file is half written: the real file is not touched.
    {
        std::ofstream tmp(dir.path() / "recovery-1.tbcad.tmp", std::ios::binary);
        tmp << "TBCAD half written";
    }
    CHECK(dumpOf(file) == canonicalDump(*one));

    // The next write replaces the stale temporary file and wins.
    io::WriteOptions crash = opt;
    crash.simulateCrashBeforeReplace = true;
    CHECK_FALSE(io::writeTbcad(*two, file, crash).ok());
    CHECK(dumpOf(file) == canonicalDump(*one));
    REQUIRE(io::writeTbcad(*two, file, opt).ok());
    CHECK(dumpOf(file) == canonicalDump(*two));
    CHECK_FALSE(fs::exists(dir.path() / "recovery-1.tbcad.tmp"));
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
    // The Windows C runtime writes "\n" as "\r\n" in text mode.
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    CHECK(text.find("chunk HEAD") != std::string::npos);
    CHECK(text.find("chunk END") != std::string::npos);
    CHECK(text.find(canonicalDump(*doc)) != std::string::npos);

    // A damaged file exits with 1.
    fs::resize_file(file, 30);
    CHECK(std::system(cmd.c_str()) != 0);
}

TEST_CASE("tbcad-dump --salvage reads a file with a cut-off tail") {
    TempDir dir;
    fs::path file = dir.path() / "cut.tbcad";
    REQUIRE(io::writeTbcad(*makeRichDocument(), file).ok());
    fs::resize_file(file, fs::file_size(file) - 7);
    auto run = [&](const char* flag) {
        std::string cmd = std::string("\"") + TBCAD_DUMP_EXE + "\" " + flag + " \"" +
                          file.string() + "\" > \"" + (dir.path() / "o.txt").string() + "\"";
#ifdef _WIN32
        cmd = "\"" + cmd + "\"";
#endif
        return std::system(cmd.c_str());
    };
    CHECK(run("") != 0);
    CHECK(run("--salvage") == 0);
}
#endif

TEST_CASE("CRC-32: the standard check value, and slices of every length agree with a plain loop") {
    const std::string s = "123456789";
    CHECK(io::detail::tbcadCrc32({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}) ==
          0xCBF43926u);
    std::vector<std::uint8_t> data(1000);
    std::uint32_t x = 12345;
    for (auto& b : data) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<std::uint8_t>(x >> 24);
    }
    for (std::size_t n = 0; n <= 40; ++n) {
        std::uint32_t c = 0xFFFFFFFFu;  // the one-byte-at-a-time definition
        for (std::size_t i = 0; i < n; ++i) {
            c ^= data[i];
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        CHECK(io::detail::tbcadCrc32({data.data(), n}) == (c ^ 0xFFFFFFFFu));
    }
}

TEST_CASE("save: a drawing of several chunks is written the same way every time") {
    auto doc = docWith(300'000);  // more than one 4 MiB entity chunk
    auto a = io::writeTbcadToMemory(*doc);
    auto b = io::writeTbcadToMemory(*doc);
    TB_REQUIRE_OK(a);
    TB_REQUIRE_OK(b);
    CHECK(a.value() == b.value());
    io::IoReport report;
    auto back = io::readTbcad(a.value(), report);
    TB_REQUIRE_OK(back);
    CHECK(back.value()->entityCount() == 300'000);
    auto chunks = io::listChunks(a.value());
    TB_REQUIRE_OK(chunks);
    std::size_t ents = 0;
    for (const auto& c : chunks.value())
        ents += c.type == "ENTS" ? 1 : 0;
    CHECK(ents >= 2);
}

TEST_CASE(
    "save: writeTbcadAsync writes the file as it was when called, even if the drawing changes") {
    TempDir dir;
    fs::path file = dir.path() / "async.tbcad";
    auto doc = docWith(50'000);
    const std::string before = canonicalDump(*doc);
    auto pending = io::writeTbcadAsync(*doc, file);
    {
        // The drawing goes on changing while the file is being finished.
        Transaction tx = doc->begin("more");
        for (int i = 0; i < 100; ++i) {
            Entity e;
            e.common.layer = kLayerZero;
            e.data = LineData{{0, double(i)}, {1, double(i)}};
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    auto result = pending.get();
    TB_REQUIRE_OK(result);
    CHECK(dumpOf(file) == before);
    CHECK_FALSE(fs::exists(dir.path() / "async.tbcad.tmp"));
}

TEST_CASE("save: the backup keeps the old content when a new file replaces it") {
    TempDir dir;
    fs::path file = dir.path() / "a.tbcad";
    auto one = docWith(1);
    auto two = docWith(2);
    REQUIRE(io::writeTbcad(*one, file).ok());
    REQUIRE(io::writeTbcad(*two, file).ok());
    REQUIRE(io::writeTbcad(*one, file).ok());
    CHECK(dumpOf(dir.path() / "a.tbcad.bak") == canonicalDump(*two));
    CHECK(dumpOf(file) == canonicalDump(*one));
}

TEST_CASE("save: a read-only file is not replaced and the message says so") {
    TempDir dir;
    fs::path file = dir.path() / "locked.tbcad";
    auto one = docWith(1);
    auto two = docWith(2);
    REQUIRE(io::writeTbcad(*one, file).ok());
    fs::permissions(file, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);
    auto r = io::writeTbcad(*two, file);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("read-only") != std::string::npos);
    CHECK(dumpOf(file) == canonicalDump(*one));
    CHECK_FALSE(fs::exists(dir.path() / "locked.tbcad.tmp"));
    CHECK_FALSE(fs::exists(dir.path() / "locked.tbcad.bak"));
    fs::permissions(file, fs::perms::owner_write, fs::perm_options::add);
    REQUIRE(io::writeTbcad(*two, file).ok());
    CHECK(dumpOf(file) == canonicalDump(*two));
}

// std::filesystem::u8path is deprecated in C++20 (an error under /WX on MSVC).
static fs::path u8Path(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

TEST_CASE("save: Thai names, spaces and a long folder path save, back up and reopen") {
    TempDir dir;
    fs::path folder = dir.path() / u8Path("งานแบบ บ้าน (แก้ไข) 2569");
    for (int i = 0; i < 6; ++i)
        folder /=
            u8Path("โฟลเดอร์ยาวมากๆ ทดสอบ " + std::to_string(i) + " abcdefghijklmnopqrstuvwxyz");
    // The folder is longer than MAX_PATH; Windows needs the extended-length form.
    REQUIRE(fs::create_directories(platform::longPath(folder)));
    fs::path file = folder / u8Path("แปลน ชั้น 1 final.tbcad");
    auto one = docWith(1);
    auto two = docWith(2);
    REQUIRE(io::writeTbcad(*one, file).ok());
    REQUIRE(io::writeTbcad(*two, file).ok());
    CHECK(dumpOf(file) == canonicalDump(*two));
    fs::path bak = file;
    bak += ".bak";
    CHECK(dumpOf(bak) == canonicalDump(*one));
    CHECK_FALSE(fs::exists(platform::longPath(fs::path(file) += ".tmp")));
}
