// Reading with a second thread that splits the text into records gives the same drawing and the
// same diagnostics, in the same order, as reading on one thread: on the corpus and on damaged copies.
#include <algorithm>
#include <cstdint>
#include <filesystem>

#include "../lexer/doctest_include.h"
#include "../lexer/test_util.h"
#include "../writer/semantic_dump.h"
#include "tbcad/dxf/reader.h"

namespace tbcad::dxf {
namespace {

struct Rng {
    std::uint64_t s = 0x2545F4914F6CDD1Dull;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

// Both ways of reading `bytes` agree.
void expectSame(const std::vector<std::byte>& bytes) {
    ImportOptions one, two;
    one.maxThreads = 1;
    two.maxThreads = 2;
    Report ra, rb;
    auto a = importDxf(bytes, one, ra);
    auto b = importDxf(bytes, two, rb);
    REQUIRE(a.ok() == b.ok());
    CHECK(ra.toJson() == rb.toJson());
    CHECK(ra.count(Severity::Info) == rb.count(Severity::Info));
    CHECK(ra.count(Severity::Warning) == rb.count(Severity::Warning));
    CHECK(ra.count(Severity::Error) == rb.count(Severity::Error));
    if (!a.ok()) {
        CHECK(a.error().message == b.error().message);
        return;
    }
    const std::string diff = testing::SemanticDump::diff(testing::SemanticDump(*a.value().document),
                                                         testing::SemanticDump(*b.value().document));
    CHECK_MESSAGE(diff.empty(), diff);
}

std::vector<std::filesystem::path> files() {
    std::vector<std::filesystem::path> out;
    for (const char* dir : {"reader", "roundtrip", "synthetic/lexer"}) {
        const auto root = std::filesystem::path(TBCAD_TEST_CORPUS_DIR) / dir;
        if (!std::filesystem::exists(root))
            continue;
        for (const auto& e : std::filesystem::directory_iterator(root))
            if (e.path().extension() == ".dxf")
                out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("reading on two threads gives the same drawing and report on every corpus file") {
    const auto all = files();
    REQUIRE(all.size() >= 40);
    for (const auto& path : all) {
        INFO("file: " << path.string());
        expectSame(testing::readFile(path.string()));
    }
}

TEST_CASE("reading on two threads gives the same report on damaged files") {
    const auto all = files();
    Rng rng;
    std::vector<std::vector<std::byte>> seeds;
    for (const auto& p : all)
        seeds.push_back(testing::readFile(p.string()));
    for (int iter = 0; iter < 400; ++iter) {
        std::vector<std::byte> data = seeds[rng.below(seeds.size())];
        const int edits = 1 + static_cast<int>(rng.below(6));
        for (int k = 0; k < edits && !data.empty(); ++k) {
            switch (rng.below(4)) {
                case 0:
                    data[rng.below(data.size())] = static_cast<std::byte>(rng.next());
                    break;
                case 1:
                    data.resize(rng.below(data.size() + 1));
                    break;
                case 2: {
                    const std::size_t at = rng.below(data.size());
                    data.erase(data.begin() + static_cast<std::ptrdiff_t>(at),
                               data.begin() + static_cast<std::ptrdiff_t>(
                                                  std::min(data.size(), at + 1 + rng.below(16))));
                    break;
                }
                default:
                    data.insert(data.begin() + static_cast<std::ptrdiff_t>(rng.below(data.size())),
                                static_cast<std::byte>("\r\n 0123abxyz.-"[rng.below(14)]));
                    break;
            }
        }
        INFO("iteration " << iter);
        expectSame(data);
    }
}

TEST_CASE("a file with more problems than the report keeps reads the same on two threads") {
    std::string text = "  0\nSECTION\n  2\nENTITIES\n";
    for (int i = 0; i < 4000; ++i)  // many records, one problem each
        text += "  0\nLINE\n 10\nabc\n 20\n1.5\n 11\n2\n 21\n3\n";
    text += "  0\nLINE\n";  // one record with more problems than a report keeps
    for (int i = 0; i < 6000; ++i)
        text += " 10\nabc\n";
    text += "  0\nENDSEC\n  0\nEOF\n";
    expectSame(testing::bytesOf(text));
}

}  // namespace tbcad::dxf
