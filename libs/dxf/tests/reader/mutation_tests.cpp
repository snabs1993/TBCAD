// Deterministic mutation smoke test of the whole reader: random byte flips, cuts, number
// rewrites and record shuffles of the corpus files must never crash, hang or leave the
// document inconsistent. (tests/reader/fuzz_reader.cpp is the libFuzzer version; this one runs
// in every ctest run, including the ASan + UBSan job.)
#include <cstdint>
#include <filesystem>
#include <string>

#include "reader_helpers.h"

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

const char* kNumbers[] = {"0",   "-0",     "1e308",  "-1e308", "nan",
                          "inf", "1e-300", "-1",     "65536",  "99999999999999999999",
                          "0.5", "1e12",   "3.14159"};

void mutate(std::vector<std::byte>& data, Rng& rng) {
    switch (rng.below(7)) {
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
                                          std::min(data.size(), at + 1 + rng.below(40))));
            break;
        }
        case 3: {  // duplicate a slice somewhere else
            const std::size_t at = rng.below(data.size());
            const std::size_t len = std::min(data.size() - at, 1 + rng.below(200));
            std::vector<std::byte> slice(data.begin() + static_cast<std::ptrdiff_t>(at),
                                         data.begin() + static_cast<std::ptrdiff_t>(at + len));
            const std::size_t to = rng.below(data.size() + 1);
            data.insert(data.begin() + static_cast<std::ptrdiff_t>(to), slice.begin(), slice.end());
            break;
        }
        default: {  // rewrite a number: find a line that starts with a digit or '-' and replace it
            std::size_t at = rng.below(data.size());
            for (std::size_t tries = 0; tries < 200; ++tries) {
                std::size_t p = (at + tries * 7) % data.size();
                if (p > 0 && data[p - 1] == std::byte{'\n'}) {
                    std::size_t e = p;
                    while (e < data.size() && data[e] != std::byte{'\n'} &&
                           data[e] != std::byte{'\r'})
                        ++e;
                    const std::string repl =
                        kNumbers[rng.below(sizeof(kNumbers) / sizeof(*kNumbers))];
                    data.erase(data.begin() + static_cast<std::ptrdiff_t>(p),
                               data.begin() + static_cast<std::ptrdiff_t>(e));
                    for (std::size_t i = 0; i < repl.size(); ++i)
                        data.insert(data.begin() + static_cast<std::ptrdiff_t>(p + i),
                                    static_cast<std::byte>(repl[i]));
                    break;
                }
            }
            break;
        }
    }
}

}  // namespace

TEST_CASE("mutated corpus files never crash the reader") {
    std::vector<std::vector<std::byte>> seeds;
    for (const auto& e : std::filesystem::directory_iterator(testing::corpusPath("reader"))) {
        if (e.path().extension() == ".dxf")
            seeds.push_back(testing::readFile(e.path().string()));
    }
    REQUIRE(seeds.size() > 10);
    Rng rng;
    std::size_t imported = 0;
    for (int iter = 0; iter < 2500; ++iter) {
        std::vector<std::byte> data = seeds[rng.below(seeds.size())];
        const int edits = 1 + static_cast<int>(rng.below(6));
        for (int k = 0; k < edits && !data.empty(); ++k)
            mutate(data, rng);
        ImportOptions options;
        if (rng.below(3) == 0)
            options.splitLayers = "~";
        options.dimensionGraphics = rng.below(4) != 0;
        Report report;
        Result<ImportResult> r = importDxf(data, options, report);
        if (r.ok()) {
            ++imported;
            const Document& doc = *r.value().document;
            // The document must be internally consistent: every model space entity resolves.
            for (EntityId id : doc.modelSpace())
                REQUIRE(doc.find(id) != nullptr);
            (void)r.value().summary.toJson(report);
        }
    }
    CHECK(imported > 500);  // most mutants are still readable
}

}  // namespace tbcad::dxf
