// Deterministic mutation smoke test: random byte flips, cuts and splices of the corpus files
// must never crash or hang the lexer, encoding detection or text decoding. (The libFuzzer
// target in fuzz_lexer.cpp does the real work; this keeps a cheap version in every ctest run
// and on platforms without libFuzzer.)

#include <cstdint>
#include <filesystem>

#include "doctest_include.h"
#include "tbcad/dxf/encoding.h"
#include "tbcad/dxf/lexer.h"
#include "test_util.h"

namespace tbcad::dxf {
namespace {

struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(next() % n); }
};

void exercise(const std::vector<std::byte>& bytes) {
    Report report;
    auto lx = Lexer::open(bytes, report);
    if (!lx) {
        return;
    }
    std::string text;
    std::size_t groups = 0;
    while (auto g = lx->next()) {
        if (g->type() == ValueType::String) {
            text += g->str();
        }
        // Every group consumes input, so the count is bounded by the size.
        REQUIRE(++groups <= bytes.size() + 1);
    }
    const HeaderHints hints = scanHeaderHints(bytes);
    const Encoding enc = detectEncoding(bytes, hints.acadVer, hints.dwgCodepage, report);
    const std::string utf8 = decodeText(text, enc);
    (void)stripMText(utf8);
    (void)report.toJson();
}

}  // namespace

TEST_CASE("mutated corpus files never crash the lexer or the text decoder") {
    std::vector<std::vector<std::byte>> seeds;
    for (const auto& e :
         std::filesystem::directory_iterator(testing::corpusPath("synthetic/lexer"))) {
        if (e.path().extension() == ".dxf") {
            seeds.push_back(testing::readFile(e.path().string()));
        }
    }
    REQUIRE(!seeds.empty());
    Rng rng;
    for (int iter = 0; iter < 6000; ++iter) {
        std::vector<std::byte> data = seeds[rng.below(seeds.size())];
        const int edits = 1 + static_cast<int>(rng.below(8));
        for (int k = 0; k < edits && !data.empty(); ++k) {
            switch (rng.below(5)) {
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
                case 3: {
                    const std::size_t at = rng.below(data.size());
                    const std::size_t len = std::min(data.size() - at, 1 + rng.below(32));
                    data.insert(data.begin() + static_cast<std::ptrdiff_t>(rng.below(data.size())),
                                data.begin() + static_cast<std::ptrdiff_t>(at),
                                data.begin() + static_cast<std::ptrdiff_t>(at + len));
                    break;
                }
                default:
                    data.insert(data.begin() + static_cast<std::ptrdiff_t>(rng.below(data.size())),
                                static_cast<std::byte>("\r\n\\%{};+-.,0"[rng.below(12)]));
                    break;
            }
        }
        exercise(data);
    }
}

}  // namespace tbcad::dxf
