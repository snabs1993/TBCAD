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

TEST_CASE("sheet list: a page setup column, Model, and writing the list back") {
    auto r = parseSheetList("a.tbcad | Plan | Half\nb.tbcad | | Site\nc.tbcad | Model\n", "/proj");
    REQUIRE(r.ok());
    REQUIRE(r.value().size() == 3);
    CHECK(r.value()[0] == SheetEntry{"/proj/a.tbcad", "Plan", "Half"});
    CHECK(r.value()[1] == SheetEntry{"/proj/b.tbcad", "", "Site"});
    CHECK(r.value()[2] == SheetEntry{"/proj/c.tbcad", "", ""});
    const std::vector<SheetEntry> list{{"/proj/a.tbcad", "Plan", "Half"},
                                       {"/proj/sub/b.tbcad", "", ""},
                                       {"/elsewhere/c.tbcad", "", "Site"}};
    const std::string text = formatSheetList(list, "/proj");
    CHECK(text.find("a.tbcad | Plan | Half\n") != std::string::npos);
    CHECK(text.find("\nsub/b.tbcad\n") != std::string::npos);
    CHECK(text.find("/elsewhere/c.tbcad |  | Site\n") != std::string::npos);
    auto back = parseSheetList(text, "/proj");
    REQUIRE(back.ok());
    CHECK(back.value() == list);
}

TEST_CASE("sheet list: page setup override, the open drawing, and a decorate hook") {
    const fs::path dir = fs::temp_directory_path() / "tbcad_sheets_override";
    fs::create_directories(dir);
    save(dir / "a.tbcad", true);
    Document source;  // the publishing drawing holds the setup
    {
        auto tx = source.begin("setup");
        layout::PageSetup mono;
        mono.name = "Mono";
        mono.settings.color = exportlib::ColorMode::Monochrome;
        REQUIRE(layout::savePageSetup(tx, mono).ok());
        tx.commit();
    }
    std::vector<SheetEntry> entries{{dir / "a.tbcad", "Plan", "Mono"}};
    exportlib::PlotSettings base;
    CHECK_FALSE(buildSheetList(entries, base).ok());  // not in the sheet's drawing
    BuildOptions opts;
    opts.setupSource = &source;
    int decorated = 0;
    opts.decorate = [&](exportlib::PlotSheet&, const SheetEntry& e,
                        const exportlib::PlotSettings& s) {
        ++decorated;
        CHECK(e.layout == "Plan");
        CHECK(s.color == exportlib::ColorMode::Monochrome);
    };
    REQUIRE(buildSheetList(entries, base, {}, opts).ok());
    CHECK(decorated == 1);

    // The open drawing is used as it is (here: a layout the saved file does not have).
    Document open;
    {
        auto tx = open.begin("open");
        REQUIRE(tx.add(line({0, 0}, {10, 10})).ok());
        REQUIRE(layout::createLayout(tx, "Fresh", "A4").ok());
        tx.commit();
    }
    BuildOptions cur;
    cur.current = &open;
    cur.currentFile = dir / "a.tbcad";
    const std::vector<SheetEntry> fresh{{dir / "a.tbcad", "Fresh", ""}};
    CHECK_FALSE(buildSheetList(fresh, base).ok());
    CHECK(buildSheetList(fresh, base, {}, cur).ok());
    const auto all = sheetsOf(open, dir / "a.tbcad");
    REQUIRE(all.size() == 2);
    CHECK(all[0].layout.empty());
    CHECK(all[1].layout == "Fresh");
    fs::remove_all(dir);
}
