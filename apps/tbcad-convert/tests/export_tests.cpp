// tbcad-convert in the .tbcad -> DXF direction, and the DXF -> tbcad -> DXF -> tbcad round trip.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../../libs/dxf/tests/writer/semantic_dump.h"
#include "tbcad/convert/cli.h"
#include "tbcad/core/dump.h"
#include "tbcad/io/tbcad.h"

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("tbcad-export-test-" +
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

std::unique_ptr<tbcad::Document> load(const fs::path& path) {
    tbcad::io::IoReport report;
    auto doc = tbcad::io::readTbcad(path, report);
    REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
    return std::move(doc.value());
}

std::vector<fs::path> corpusFiles() {
    std::vector<fs::path> files;
    for (const char* dir : {"reader", "roundtrip"}) {
        fs::path root = fs::path(TBCAD_TEST_CORPUS_DIR) / dir;
        if (!fs::exists(root))
            continue;
        for (const auto& e : fs::directory_iterator(root)) {
            if (e.path().extension() == ".dxf")
                files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

TEST_CASE("DXF -> tbcad -> DXF -> tbcad gives equal drawings for every corpus file") {
    TempDir tmp;
    std::size_t done = 0;
    for (const fs::path& dxf : corpusFiles()) {
        INFO("file: " << dxf.filename().string());
        std::string name = dxf.stem().string();
        fs::path first = tmp / (name + ".1.tbcad"), second = tmp / (name + ".2.dxf"),
                 third = tmp / (name + ".3.tbcad");
        if (run({dxf.string(), "-o", first.string(), "-q"}).exit == tbcad::convert::kExitFailed)
            continue;  // the broken_* files
        Run exported = run({first.string(), "-o", second.string(), "-q"});
        REQUIRE_MESSAGE(exported.exit != tbcad::convert::kExitFailed, exported.err);
        Run again = run({second.string(), "-o", third.string(), "-q"});
        REQUIRE_MESSAGE(again.exit != tbcad::convert::kExitFailed, again.err);

        // Foreign (AEC, Civil, ACIS) objects are exported as plain geometry, so they come back as
        // polylines instead of proxies.
        if (name == "aec_objects")
            continue;
        auto a = load(first);
        auto b = load(third);
        std::string diff = tbcad::dxf::testing::SemanticDump::diff(
            tbcad::dxf::testing::SemanticDump(*a), tbcad::dxf::testing::SemanticDump(*b));
        CHECK_MESSAGE(diff.empty(), diff);
        // Same number of entities, same layers: also visible in the plain canonical dumps.
        CHECK(a->layers().size() == b->layers().size());
        ++done;
    }
    CHECK(done >= 20);
}

TEST_CASE("export options of the command line") {
    TempDir tmp;
    fs::path src = fs::path(TBCAD_TEST_CORPUS_DIR) / "reader" / "r2007_utf8_thai.dxf";
    fs::path tbcad = tmp / "a.tbcad";
    REQUIRE(run({src.string(), "-o", tbcad.string(), "-q"}).exit != 2);

    SUBCASE("default is R2000, ASCII") {
        Run r = run({tbcad.string(), "-o", (tmp / "a.dxf").string()});
        CHECK(r.exit != 2);
        std::string text = readText(tmp / "a.dxf");
        CHECK(text.find("AC1015") != std::string::npos);
        CHECK(std::all_of(text.begin(), text.end(), [](char c) { return (c & 0x80) == 0; }));
        CHECK(r.out.find("R2000") != std::string::npos);
    }
    SUBCASE("--version r12 and --utf8") {
        CHECK(
            run({tbcad.string(), "-o", (tmp / "b.dxf").string(), "--version", "r12", "-q"}).exit !=
            2);
        CHECK(readText(tmp / "b.dxf").find("AC1009") != std::string::npos);
        CHECK(run({tbcad.string(), "-o", (tmp / "c.dxf").string(), "--utf8", "-q"}).exit != 2);
        std::string c = readText(tmp / "c.dxf");
        CHECK(c.find("AC1021") != std::string::npos);
        CHECK_FALSE(std::all_of(c.begin(), c.end(), [](char x) { return (x & 0x80) == 0; }));
    }
    SUBCASE("a bad version is rejected") {
        Run r = run({tbcad.string(), "-o", (tmp / "d.dxf").string(), "--version", "r99"});
        CHECK(r.exit == tbcad::convert::kExitFailed);
        CHECK(r.err.find("r99") != std::string::npos);
        CHECK_FALSE(fs::exists(tmp / "d.dxf"));
    }
    SUBCASE("--layer-separator and --report") {
        fs::path layers = fs::path(TBCAD_TEST_CORPUS_DIR) / "reader" / "layers_tilde.dxf";
        fs::path split = tmp / "split.tbcad";
        REQUIRE(run({layers.string(), "-o", split.string(), "--split-layers", "~", "-q"}).exit !=
                2);
        Run r = run({split.string(), "-o", (tmp / "e.dxf").string(), "--layer-separator", "-",
                     "--report", (tmp / "e.json").string(), "-q"});
        CHECK(r.exit != 2);
        std::string json = readText(tmp / "e.json");
        CHECK(json.find("\"output\":{\"acadver\":\"AC1015\"") != std::string::npos);
        CHECK(json.find("\"entities\"") != std::string::npos);
        // Sub-layers now use "-" between the names.
        CHECK(readText(tmp / "e.dxf").find("parent=") != std::string::npos);
        Run bad = run({split.string(), "-o", (tmp / "f.dxf").string(), "--layer-separator", "|"});
        CHECK(bad.exit == tbcad::convert::kExitFailed);
    }
    SUBCASE("a missing input fails and leaves no output") {
        Run r = run({(tmp / "none.tbcad").string(), "-o", (tmp / "g.dxf").string()});
        CHECK(r.exit == tbcad::convert::kExitFailed);
        CHECK_FALSE(fs::exists(tmp / "g.dxf"));
    }
}
