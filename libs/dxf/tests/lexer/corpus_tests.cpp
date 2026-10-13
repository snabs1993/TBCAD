// Runs the lexer over every tests/corpus/synthetic/lexer/*.dxf and compares the token
// stream and warnings with the neighbouring .expected.txt (see tools/make_lexer_corpus.py).

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "doctest_include.h"
#include "tbcad/dxf/lexer.h"
#include "test_util.h"

namespace tbcad::dxf {
namespace {

std::string escape(std::string_view raw) {
    std::string out;
    for (const char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out.push_back(ch);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string describe(const Group& g) {
    std::string v;
    char letter = 'S';
    switch (g.type()) {
        case ValueType::String:
            v = escape(g.str());
            break;
        case ValueType::Double: {
            char buf[64];
            const auto r = std::to_chars(buf, buf + sizeof buf, g.number());
            v.assign(buf, r.ptr);
            letter = 'D';
            break;
        }
        case ValueType::Int16:
        case ValueType::Int32:
        case ValueType::Int64:
            v = std::to_string(g.integer());
            letter = 'I';
            break;
        case ValueType::Bool:
            v = g.integer() != 0 ? "1" : "0";
            letter = 'B';
            break;
        case ValueType::Handle:
            v = std::string(g.str());
            letter = 'H';
            break;
        case ValueType::Binary: {
            for (const std::byte b : std::get<std::vector<std::byte>>(g.value)) {
                char buf[4];
                std::snprintf(buf, sizeof buf, "%02X", static_cast<unsigned>(b));
                v += buf;
            }
            letter = 'X';
            break;
        }
    }
    return "G " + std::to_string(g.line) + " " + std::to_string(g.code) + " " + letter + " " + v;
}

std::vector<std::string> lex(std::span<const std::byte> bytes) {
    Report report;
    auto lx = Lexer::open(bytes, report);
    std::vector<std::string> lines;
    REQUIRE(lx.ok());
    while (auto g = lx->next()) {
        lines.push_back(describe(*g));
    }
    for (const Diagnostic& d : report.diagnostics()) {
        if (d.severity != Severity::Info) {
            lines.push_back("W " + std::to_string(d.line) + " " + d.code);
        }
    }
    return lines;
}

std::vector<std::string> readLines(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
    }
    return lines;
}

}  // namespace

TEST_CASE("lexer corpus: token streams and warnings match the expected files") {
    const std::filesystem::path dir = testing::corpusPath("synthetic/lexer");
    REQUIRE(std::filesystem::is_directory(dir));
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".dxf") {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    CHECK(files.size() >= 12);
    for (const auto& file : files) {
        CAPTURE(file.filename().string());
        auto expectedPath = file;
        expectedPath.replace_extension(".expected.txt");
        REQUIRE(std::filesystem::exists(expectedPath));
        const auto bytes = testing::readFile(file.string());
        const auto got = lex(bytes);
        const auto want = readLines(expectedPath);
        CHECK(got == want);
        if (got != want) {
            std::ostringstream os;
            for (const auto& l : got)
                os << l << '\n';
            MESSAGE("lexer output:\n" << os.str());
        }
    }
}

}  // namespace tbcad::dxf
