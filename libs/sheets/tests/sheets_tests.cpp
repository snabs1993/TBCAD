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

TEST_CASE("a layout with a page setup is plotted with that setup, others with the base settings") {
    const fs::path dir = fs::temp_directory_path() / "tbcad_sheets_pagesetup";
    fs::create_directories(dir);
    {
        Document doc;
        auto tx = doc.begin("content");
        Entity e = line({0, 0}, {5000, 2000});
        e.common.color = Color::index(1);  // red
        REQUIRE(tx.add(e).ok());
        REQUIRE(layout::createLayout(tx, "Mono", "A3").ok());
        REQUIRE(layout::createLayout(tx, "Plain", "A3").ok());
        REQUIRE(layout::addViewport(tx, "Mono", {{20, 20}, {400, 277}}, {2500, 1000}, 100).ok());
        REQUIRE(layout::addViewport(tx, "Plain", {{20, 20}, {400, 277}}, {2500, 1000}, 100).ok());
        layout::PageSetup mono;
        mono.name = "Mono";
        mono.settings.color = exportlib::ColorMode::Monochrome;
        REQUIRE(layout::savePageSetup(tx, mono).ok());
        REQUIRE(layout::assignPageSetup(tx, "Mono", "Mono").ok());
        tx.commit();
        REQUIRE(io::writeTbcad(doc, dir / "a.tbcad").ok());
    }
    auto entries = parseSheetList("a.tbcad | Mono\na.tbcad | Plain\n", dir);
    REQUIRE(entries.ok());
    exportlib::PlotSettings base;
    base.color = exportlib::ColorMode::AsDisplayed;
    auto set = buildSheetList(entries.value(), base);
    REQUIRE(set.ok());
    REQUIRE(set.value().sheets.size() == 2);
    auto firstColor = [](const exportlib::PlotSheet& s) {
        for (const auto& p : s.prims)
            if (const auto* path = std::get_if<exportlib::PlotPath>(&p))
                if (path->pts.size() == 2)
                    return path->style.color;
        return Rgba{1, 2, 3, 255};
    };
    CHECK(firstColor(set.value().sheets[0]) == Rgba{0, 0, 0, 255});
    CHECK(firstColor(set.value().sheets[1]) == Rgba{255, 0, 0, 255});
    fs::remove_all(dir);
}
