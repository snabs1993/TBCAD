// Deterministic mutation smoke test of the whole reader: random byte flips, cuts, number
// rewrites and record shuffles of the corpus files must never crash, hang or leave the
// document inconsistent. (tests/reader/fuzz_reader.cpp is the libFuzzer version; this one runs
// in every ctest run, including the ASan + UBSan job.)
#include <cstdint>
#include <cstdlib>
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

namespace {

// Splits a DXF text into (code line, value line) pairs; empty if it does not pair up.
std::vector<std::string> linesOf(const std::vector<std::byte>& data) {
    std::vector<std::string> out;
    std::string cur;
    for (std::byte b : data) {
        if (b == std::byte{'\n'}) {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(static_cast<char>(b));
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::vector<std::byte> bytesOf(const std::vector<std::string>& lines, std::size_t count) {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i < count && i < lines.size(); ++i) {
        for (char c : lines[i])
            out.push_back(static_cast<std::byte>(c));
        out.push_back(std::byte{'\n'});
    }
    return out;
}

void importQuietly(const std::vector<std::byte>& data) {
    Report report;
    Result<ImportResult> r = importDxf(data, ImportOptions{}, report);
    if (r.ok()) {
        const Document& doc = *r.value().document;
        for (EntityId id : doc.modelSpace())
            REQUIRE(doc.find(id) != nullptr);
    } else {
        CHECK_FALSE(r.error().message.empty());
    }
}

}  // namespace

TEST_CASE("a DXF cut at any record boundary gives a document or a clear error") {
    std::size_t seeds = 0;
    for (const auto& e : std::filesystem::directory_iterator(testing::corpusPath("reader"))) {
        if (e.path().extension() != ".dxf" || std::filesystem::file_size(e.path()) > 12000)
            continue;
        const auto lines = linesOf(testing::readFile(e.path().string()));
        ++seeds;
        for (std::size_t n = 0; n <= lines.size(); n += 2)
            importQuietly(bytesOf(lines, n));
        if (seeds >= 12)
            break;
    }
    CHECK(seeds > 3);
}

TEST_CASE("bad group codes and huge counts never crash or exhaust memory") {
    const char* kCounts[] = {"2147483647",  "4294967295",     "-1",
                             "-2147483648", "99999999999999", "1000000000"};
    const char* kCodes[] = {"-5", "-1", "1071", "9999", "99999999999", "abc", " ", "", "0 0"};
    std::vector<std::vector<std::byte>> seeds;
    for (const auto& e : std::filesystem::directory_iterator(testing::corpusPath("reader")))
        if (e.path().extension() == ".dxf")
            seeds.push_back(testing::readFile(e.path().string()));
    REQUIRE(seeds.size() > 10);
    Rng rng;
    for (int iter = 0; iter < 1500; ++iter) {
        auto lines = linesOf(seeds[rng.below(seeds.size())]);
        if (lines.size() < 4)
            continue;
        const int edits = 1 + static_cast<int>(rng.below(4));
        for (int k = 0; k < edits; ++k) {
            // Code lines are the even ones; the value follows.
            std::size_t at = rng.below(lines.size() / 2) * 2;
            if (rng.below(2) == 0) {
                lines[at] = kCodes[rng.below(sizeof(kCodes) / sizeof(*kCodes))];
            } else {
                // A count-like group: 70..79, 90..99, 170..179, 270..289, 90 etc.
                std::size_t tries = 0;
                while (tries++ < 400) {
                    const std::string& c = lines[at];
                    const bool countCode =
                        c.find_first_not_of(" 0123456789") == std::string::npos && !c.empty() &&
                        (std::strtol(c.c_str(), nullptr, 10) / 10 == 7 ||
                         std::strtol(c.c_str(), nullptr, 10) / 10 == 9 ||
                         std::strtol(c.c_str(), nullptr, 10) / 10 == 17);
                    if (countCode)
                        break;
                    at = (at + 2) % (lines.size() - 1 - ((lines.size() - 1) % 2));
                }
                if (at + 1 < lines.size())
                    lines[at + 1] = kCounts[rng.below(sizeof(kCounts) / sizeof(*kCounts))];
            }
        }
        importQuietly(bytesOf(lines, lines.size()));
    }
}

}  // namespace tbcad::dxf
