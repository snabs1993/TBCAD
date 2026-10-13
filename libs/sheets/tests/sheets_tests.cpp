#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/sheets/sheets.h"

using namespace tbcad;
using namespace tbcad::sheets;
namespace fs = std::filesystem;

namespace {

Entity line(Vec2 a, Vec2 b) {
    Entity e;
    e.data = LineData{a, b};
    return e;
}

void save(const fs::path& file, bool withLayout) {
    Document doc;
    auto tx = doc.begin("content");
    REQUIRE(tx.add(line({0, 0}, {5000, 2000})).ok());
    if (withLayout) {
        REQUIRE(layout::createLayout(tx, "Plan", "A3").ok());
        REQUIRE(layout::addViewport(tx, "Plan", {{20, 20}, {400, 277}}, {2500, 1000}, 100).ok());
    }
    tx.commit();
    REQUIRE(io::writeTbcad(doc, file).ok());
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("sheet list parsing: comments, layouts and relative paths") {
    auto r = parseSheetList("# title\n\n a.tbcad | Plan A \nsub/b.tbcad\n", "/proj");
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 2);
    CHECK(r.value()[0].file == fs::path("/proj/a.tbcad"));
    CHECK(r.value()[0].layout == "Plan A");
    CHECK(r.value()[1].file == fs::path("/proj/sub/b.tbcad"));
    CHECK(r.value()[1].layout.empty());
    CHECK_FALSE(parseSheetList("# nothing\n", "/proj").ok());
    CHECK_FALSE(parseSheetList(" | Plan\n", "/proj").ok());
}

TEST_CASE("a sheet list from two drawings becomes one two-page PDF") {
    const fs::path dir = fs::temp_directory_path() / "tbcad-sheets-test";
    fs::create_directories(dir);
    save(dir / "one.tbcad", true);
    save(dir / "two.tbcad", false);
    {
        std::ofstream list(dir / "set.txt");
        list << "one.tbcad | Plan\ntwo.tbcad\none.tbcad | Plan\n";
    }
    auto entries = readSheetList(dir / "set.txt");
    REQUIRE(entries.ok());
    REQUIRE(entries.value().size() == 3);

    exportlib::PlotSettings base;
    auto r = plotSheetList(entries.value(), base, dir / "set.pdf");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    CHECK(r.value().pages == 3);
    std::string pdf = slurp(dir / "set.pdf");
    std::regex page(R"(/Type\s*/Page[^s])");
    CHECK(std::distance(std::sregex_iterator(pdf.begin(), pdf.end(), page),
                        std::sregex_iterator()) == 3);

    // An unknown layout fails the whole set and names the entry.
    entries.value()[1].layout = "Nope";
    auto bad = plotSheetList(entries.value(), base, dir / "bad.pdf");
    CHECK_FALSE(bad.ok());
    CHECK_FALSE(fs::exists(dir / "bad.pdf"));
    fs::remove_all(dir);
}
