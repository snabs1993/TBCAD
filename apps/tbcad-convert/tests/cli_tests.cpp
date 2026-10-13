#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/convert/cli.h"
#include "tbcad/core/dump.h"
#include "tbcad/dxf/reader.h"
#include "tbcad/io/tbcad.h"

namespace fs = std::filesystem;

namespace {

fs::path corpus(const std::string& name) {
    return fs::path(TBCAD_TEST_CORPUS_DIR) / "reader" / name;
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("tbcad-convert-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path operator/(const std::string& n) const { return path_ / n; }

private:
    fs::path path_;
};

struct Run {
    int exit = -1;
    std::string out, err;
};

Run run(std::vector<std::string> args) {
    std::ostringstream out, err;
    Run r;
    r.exit = tbcad::convert::runCli(args, out, err);
    r.out = out.str();
    r.err = err.str();
    return r;
}

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("cli: help and usage errors") {
    CHECK(run({"--help"}).exit == tbcad::convert::kExitOk);
    CHECK(run({}).exit == tbcad::convert::kExitFailed);
    CHECK(run({"in.dxf"}).exit == tbcad::convert::kExitFailed);
    CHECK(run({"a.dxf", "b.dxf", "-o", "x.tbcad"}).exit == tbcad::convert::kExitFailed);
    CHECK(run({"a.dxf", "-o", "x.tbcad", "--bogus"}).exit == tbcad::convert::kExitFailed);
    CHECK(run({"a.dxf", "-o", "x.tbcad", "--units", "parsec-ish"}).exit ==
          tbcad::convert::kExitFailed);
    CHECK(run({"a.dxf", "-o", "x.tbcad", "--encoding", "klingon"}).exit ==
          tbcad::convert::kExitFailed);
    CHECK(run({"a.dxf", "-o"}).exit == tbcad::convert::kExitFailed);
}

TEST_CASE("cli: a clean file exits 0 and writes a readable .tbcad") {
    TempDir dir;
    Run r = run({corpus("r12_basic.dxf").string(), "-o", (dir / "a.tbcad").string()});
    CHECK(r.exit == 0);
    CHECK(r.out.find("entities") != std::string::npos);
    tbcad::io::IoReport rep;
    auto doc = tbcad::io::readTbcad(dir / "a.tbcad", rep);
    REQUIRE(doc.ok());
    CHECK(doc.value()->modelSpaceCount() == 9);
}

TEST_CASE("cli: warnings give exit 1 and a JSON report") {
    TempDir dir;
    Run r = run({corpus("unknown_entities.dxf").string(), "-o", (dir / "a.tbcad").string(),
                 "--report", (dir / "r.json").string()});
    CHECK(r.exit == tbcad::convert::kExitWarnings);
    CHECK(r.err.find("skipped") != std::string::npos);
    std::string json = readText(dir / "r.json");
    CHECK(json.find("\"3DSOLID\"") != std::string::npos);
    CHECK(json.find("dxf.reader.skipped_entity") != std::string::npos);
    CHECK(json.front() == '{');
    CHECK(json.back() == '}');
    // --quiet hides the messages but keeps the exit code
    Run q = run({corpus("unknown_entities.dxf").string(), "-o", (dir / "b.tbcad").string(), "-q"});
    CHECK(q.exit == 1);
    CHECK(q.out.empty());
    CHECK(q.err.empty());
}

TEST_CASE("cli: files that cannot be converted exit 2 and write no .tbcad") {
    TempDir dir;
    CHECK(run({corpus("broken_not_dxf.dxf").string(), "-o", (dir / "a.tbcad").string()}).exit == 2);
    CHECK_FALSE(fs::exists(dir / "a.tbcad"));
    CHECK(run({corpus("broken_old_version.dxf").string(), "-o", (dir / "a.tbcad").string()}).exit ==
          2);
    CHECK(run({(dir / "missing.dxf").string(), "-o", (dir / "a.tbcad").string()}).exit == 2);
    CHECK(run({corpus("r12_basic.dxf").string(), "-o",
               (dir / "no" / "such" / "dir" / "a.tbcad").string()})
              .exit == 2);
    // a failed run still writes the report when asked to
    Run r = run({corpus("broken_not_dxf.dxf").string(), "-o", (dir / "a.tbcad").string(),
                 "--report", (dir / "r.json").string()});
    CHECK(r.exit == 2);
    CHECK(readText(dir / "r.json").find("\"failed\":true") != std::string::npos);
}

TEST_CASE("cli: --units, --encoding and --split-layers reach the reader") {
    TempDir dir;
    // inches forced on a file that says millimetres
    CHECK(
        run({corpus("r2018_basic.dxf").string(), "-o", (dir / "u.tbcad").string(), "--units", "in"})
            .exit == 0);
    tbcad::io::IoReport rep;
    auto u = tbcad::io::readTbcad(dir / "u.tbcad", rep);
    REQUIRE(u.ok());
    bool found = false;
    u.value()->forEachEntity([&](const tbcad::Entity& e) {
        if (const auto* c = tbcad::entityAs<tbcad::CircleData>(e)) {
            CHECK(c->radius == doctest::Approx(3 * 25.4));
            found = true;
        }
    });
    CHECK(found);

    CHECK(run({corpus("thai_cp874_no_codepage.dxf").string(), "-o", (dir / "e.tbcad").string(),
               "--encoding", "cp874"})
              .exit == 0);
    auto e = tbcad::io::readTbcad(dir / "e.tbcad", rep);
    REQUIRE(e.ok());
    bool thai = false;
    e.value()->forEachEntity([&](const tbcad::Entity& en) {
        if (const auto* t = tbcad::entityAs<tbcad::TextData>(en))
            thai = t->utf8.find("ส") != std::string::npos;
    });
    CHECK(thai);

    CHECK(run({corpus("layers_tilde.dxf").string(), "-o", (dir / "s.tbcad").string(),
               "--split-layers", "~"})
              .exit == 0);
    auto s = tbcad::io::readTbcad(dir / "s.tbcad", rep);
    REQUIRE(s.ok());
    const tbcad::Layer* floor = s.value()->layerByName("S-FLOOR");
    REQUIRE(floor != nullptr);
    CHECK(s.value()->layerByName(floor->id, "edge") != nullptr);
}

TEST_CASE("cli: DXF -> .tbcad -> read equals the direct import for every corpus file") {
    TempDir dir;
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(corpus(""))) {
        if (entry.path().extension() != ".dxf")
            continue;
        fs::path out = dir / (entry.path().stem().string() + ".tbcad");
        Run r = run({entry.path().string(), "-o", out.string(), "-q"});
        if (r.exit == 2)
            continue;  // the files that are meant to fail
        REQUIRE_MESSAGE((r.exit == 0 || r.exit == 1), entry.path().filename().string());
        tbcad::io::IoReport rep;
        auto fromFile = tbcad::io::readTbcad(out, rep);
        REQUIRE_MESSAGE(fromFile.ok(), entry.path().filename().string());

        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i)
            bytes[i] = static_cast<std::byte>(raw[i]);
        tbcad::dxf::Report report;
        auto direct = tbcad::dxf::importDxf(bytes, {}, report);
        REQUIRE(direct.ok());
        CHECK_MESSAGE(tbcad::canonicalDump(*fromFile.value()) ==
                          tbcad::canonicalDump(*direct.value().document),
                      entry.path().filename().string());
        ++checked;
    }
    CHECK(checked >= 20);
}
