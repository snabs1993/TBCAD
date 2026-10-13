#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "tbcad/export/plot.h"

using namespace tbcad;
using namespace tbcad::exportlib;

namespace {

Entity line(Vec2 a, Vec2 b) {
    Entity e;
    e.data = LineData{a, b};
    return e;
}

void addEntity(Document& doc, Entity e) {
    auto tx = doc.begin("add");
    REQUIRE(tx.add(std::move(e)).ok());
    tx.commit();
}

// First "x y m" and "x y l" of the page content (millimetres, thanks to the page-level cm).
struct Segment {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
Segment firstSegment(const std::string& pdf) {
    std::smatch m;
    std::regex re(R"(([-\d.]+) ([-\d.]+) m\s+([-\d.]+) ([-\d.]+) l)");
    REQUIRE(std::regex_search(pdf, m, re));
    return {std::stod(m[1]), std::stod(m[2]), std::stod(m[3]), std::stod(m[4])};
}

#ifdef _WIN32
constexpr const char* kNull = "NUL";
#else
constexpr const char* kNull = "/dev/null";
#endif

void checkWithQpdf(const PlotSheet& sheet, const char* name) {
    static const bool available =
        std::system((std::string("qpdf --version > ") + kNull + " 2>&1").c_str()) == 0;
    if (!available) {
        if (std::getenv("TBCAD_REQUIRE_QPDF"))
            FAIL("qpdf not installed but TBCAD_REQUIRE_QPDF is set");
        MESSAGE("qpdf not found, skipping check for ", std::string(name));
        return;
    }
    auto path =
        std::filesystem::temp_directory_path() / (std::string("tbcad_plot_") + name + ".pdf");
    REQUIRE(writePdf(sheet, path.string()).ok());
    std::string cmd = "qpdf --check \"" + path.string() + "\" > " + kNull + " 2>&1";
    int rc = std::system(cmd.c_str());
    std::filesystem::remove(path);
    CHECK_MESSAGE(rc == 0, "qpdf --check failed for ", name);
}

// Minimal well-formedness check: balanced tags, quoted attributes, one root element.
bool wellFormedXml(const std::string& xml) {
    std::vector<std::string> stack;
    std::size_t i = 0, roots = 0;
    while ((i = xml.find('<', i)) != std::string::npos) {
        std::size_t end = xml.find('>', i);
        if (end == std::string::npos)
            return false;
        std::string tag = xml.substr(i + 1, end - i - 1);
        i = end + 1;
        if (tag.empty())
            return false;
        if (tag[0] == '?' || tag[0] == '!')
            continue;
        if (tag[0] == '/') {
            if (stack.empty() || stack.back() != tag.substr(1))
                return false;
            stack.pop_back();
            continue;
        }
        bool selfClose = tag.back() == '/';
        std::string name = tag.substr(0, tag.find_first_of(" \t\n/"));
        if (std::count(tag.begin(), tag.end(), '"') % 2)
            return false;
        if (stack.empty() && ++roots > 1)
            return false;
        if (!selfClose)
            stack.push_back(name);
    }
    return stack.empty() && roots == 1;
}

}  // namespace

TEST_CASE("A3 landscape 1:100: a 10,000 mm line is 100.00 mm on the page") {
    Document doc;
    addEntity(doc, line({0, 0}, {10000, 0}));
    PlotSettings s;
    s.paperName = "A3";
    s.fitToPaper = false;
    s.scaleN = 100;
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    CHECK(sheet.value().layout.pageWidthMm == doctest::Approx(420));
    CHECK(sheet.value().layout.pageHeightMm == doctest::Approx(297));
    auto pdf = pdfBytes(sheet.value());
    REQUIRE(pdf.ok());
    Segment seg = firstSegment(pdf.value());
    CHECK(std::abs((seg.x1 - seg.x0) - 100.0) <= 0.01);
    CHECK(std::abs(seg.y1 - seg.y0) <= 0.01);
    // Centred in the printable area.
    CHECK(seg.x0 == doctest::Approx(160).epsilon(0.001));
    CHECK(seg.y0 == doctest::Approx(148.5).epsilon(0.001));
    CHECK(pdf.value().find("/MediaBox") != std::string::npos);
    checkWithQpdf(sheet.value(), "a3_1_100");
}

TEST_CASE("fit to paper picks the scale from the extent") {
    Document doc;
    addEntity(doc, line({0, 0}, {2000, 1000}));
    PlotSettings s;
    s.fitToPaper = true;  // A3 landscape: printable 400 x 277
    auto lay = computeLayout(doc, s);
    REQUIRE(lay.ok());
    CHECK(lay.value().paperPerUnit == doctest::Approx(0.2));
    CHECK_FALSE(lay.value().overflows);
    // The scaled extent (400 x 200) is centred vertically.
    CHECK(lay.value().origin.x == doctest::Approx(10));
    CHECK(lay.value().origin.y == doctest::Approx(10 + (277 - 200) / 2.0));

    s.paperName = "A4";
    s.landscape = false;  // printable 190 x 277: width limits
    lay = computeLayout(doc, s);
    REQUIRE(lay.ok());
    CHECK(lay.value().paperPerUnit == doctest::Approx(190.0 / 2000));
}

TEST_CASE("fixed scale larger than the paper reports overflow; offset places the drawing") {
    Document doc;
    addEntity(doc, line({0, 0}, {1000, 500}));
    PlotSettings s;
    s.fitToPaper = false;
    s.scaleN = 1;
    CHECK(computeLayout(doc, s).value().overflows);
    s.scaleN = 10;
    s.centerOnPaper = false;
    s.offsetMm = {5, 7};
    auto lay = computeLayout(doc, s).value();
    CHECK_FALSE(lay.overflows);
    CHECK(lay.origin.x == doctest::Approx(15));
    CHECK(lay.origin.y == doctest::Approx(17));
}

TEST_CASE("plot area window and errors") {
    Document doc;
    PlotSettings s;
    CHECK_FALSE(computeLayout(doc, s).ok());  // nothing to plot
    addEntity(doc, line({0, 0}, {100, 100}));
    s.area = PlotArea::Window;
    CHECK_FALSE(computeLayout(doc, s).ok());  // empty window
    s.window.min = {0, 0};
    s.window.max = {50, 50};
    auto lay = computeLayout(doc, s);
    REQUIRE(lay.ok());
    CHECK(lay.value().paperPerUnit == doctest::Approx(277.0 / 50));
    s.paperName = "B7";
    CHECK_FALSE(computeLayout(doc, s).ok());
}

TEST_CASE("monochrome turns red black; white is always black; greyscale") {
    CHECK(plotColor(Rgba{255, 0, 0, 255}, ColorMode::Monochrome) == Rgba{0, 0, 0, 255});
    CHECK(plotColor(Rgba{255, 0, 0, 255}, ColorMode::AsDisplayed) == Rgba{255, 0, 0, 255});
    CHECK(plotColor(Rgba{255, 255, 255, 255}, ColorMode::AsDisplayed) == Rgba{0, 0, 0, 255});
    Rgba g = plotColor(Rgba{255, 0, 0, 255}, ColorMode::Greyscale);
    CHECK(g.r == g.g);
    CHECK(g.g == g.b);
    CHECK(g.r > 0);

    Document doc;
    Entity e = line({0, 0}, {100, 0});
    e.common.color = Color::index(1);  // red
    addEntity(doc, e);
    PlotSettings s;
    s.color = ColorMode::Monochrome;
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    auto pdf = pdfBytes(sheet.value()).value();
    CHECK(pdf.find("0 0 0 RG") != std::string::npos);
    CHECK(pdf.find("1 0 0 RG") == std::string::npos);
    s.color = ColorMode::AsDisplayed;
    pdf = pdfBytes(buildSheet(doc, s).value()).value();
    CHECK(pdf.find("1 0 0 RG") != std::string::npos);
}

TEST_CASE("lineweights are absolute on paper; dashes scale with the plot scale") {
    Document doc;
    {
        auto tx = doc.begin("lt");
        Linetype lt;
        lt.name = "DASHED";
        lt.pattern = {10, -5};
        auto id = tx.addLinetype(lt);
        REQUIRE(id.ok());
        Entity e = line({0, 0}, {10000, 0});
        e.common.linetype = id.value();
        e.common.lineweight = LineWeight::mm(0.5);
        REQUIRE(tx.add(e).ok());
        tx.commit();
    }
    PlotSettings s;
    s.fitToPaper = false;
    s.scaleN = 100;
    auto sheet = buildSheet(doc, s).value();
    REQUIRE(sheet.prims.size() == 1);
    const auto& p = std::get<PlotPath>(sheet.prims[0]);
    CHECK(p.style.widthMm == doctest::Approx(0.5));
    REQUIRE(p.style.dashMm.size() == 2);
    CHECK(p.style.dashMm[0] == doctest::Approx(0.1));
    CHECK(p.style.dashMm[1] == doctest::Approx(0.05));

    s.scaleN = 50;
    s.lineweights = false;
    sheet = buildSheet(doc, s).value();
    const auto& q = std::get<PlotPath>(sheet.prims[0]);
    CHECK(q.style.widthMm == doctest::Approx(s.hairlineMm));
    CHECK(q.style.dashMm[0] == doctest::Approx(0.2));
}

TEST_CASE("circles and arcs stay arcs: Béziers in PDF, arc commands in SVG") {
    Document doc;
    Entity c;
    c.data = CircleData{{0, 0}, 500};
    addEntity(doc, c);
    Entity a;
    a.data = ArcData{{2000, 0}, 300, 0, kPi / 2};
    addEntity(doc, a);
    PlotSettings s;
    auto sheet = buildSheet(doc, s).value();
    REQUIRE(sheet.prims.size() == 2);
    CHECK(std::holds_alternative<PlotArc>(sheet.prims[0]));
    auto pdf = pdfBytes(sheet).value();
    std::size_t curves = 0;
    for (auto pos = pdf.find(" c\n"); pos != std::string::npos; pos = pdf.find(" c\n", pos + 1))
        ++curves;
    CHECK(curves == 5);  // 4 for the circle, 1 for the quarter arc
    std::string svg = svgString(sheet);
    CHECK(svg.find("<circle") != std::string::npos);
    CHECK(svg.find(" A") != std::string::npos);
    CHECK(wellFormedXml(svg));
}

TEST_CASE("layers with plot off, off layers and hidden entities are skipped") {
    Document doc;
    LayerId noPlot;
    {
        auto tx = doc.begin("layers");
        Layer l;
        l.name = "Defpoints";
        l.plot = false;
        noPlot = tx.addLayer(l).value();
        Entity hidden = line({0, 0}, {50, 0});
        hidden.common.layer = noPlot;
        REQUIRE(tx.add(hidden).ok());
        Entity invisible = line({0, 5}, {50, 5});
        invisible.common.visible = false;
        REQUIRE(tx.add(invisible).ok());
        REQUIRE(tx.add(line({0, 10}, {50, 10})).ok());
        tx.commit();
    }
    PlotSettings s;
    auto sheet = buildSheet(doc, s).value();
    CHECK(sheet.entityCount == 1);
    CHECK(sheet.prims.size() == 1);
    // The extents ignore the skipped entities, too.
    CHECK(sheet.layout.area.height() == doctest::Approx(0));
    CHECK(sheet.layout.area.min.y == doctest::Approx(10));
}

TEST_CASE("SVG: well-formed XML in millimetres") {
    Document doc;
    addEntity(doc, line({0, 0}, {10000, 5000}));
    PlotSettings s;
    s.paperName = "A4";
    auto sheet = buildSheet(doc, s).value();
    std::string svg = svgString(sheet);
    CHECK(wellFormedXml(svg));
    CHECK(svg.find("width=\"297mm\"") != std::string::npos);
    CHECK(svg.find("height=\"210mm\"") != std::string::npos);
    CHECK(svg.find("viewBox=\"0 0 297 210\"") != std::string::npos);

    auto path = std::filesystem::temp_directory_path() / "tbcad_plot_test.svg";
    REQUIRE(writeSvg(sheet, path.string()).ok());
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str() == svg);
    std::filesystem::remove(path);
}

TEST_CASE("culling: entities outside a window are not plotted; 20,000 lines plot quickly") {
    Document doc;
    {
        auto tx = doc.begin("many");
        for (int i = 0; i < 20000; ++i)
            REQUIRE(tx.add(line({i * 10.0, 0}, {i * 10.0 + 5, 100})).ok());
        tx.commit();
    }
    PlotSettings s;
    s.area = PlotArea::Window;
    s.window.min = {0, 0};
    s.window.max = {95, 100};
    CHECK(buildSheet(doc, s).value().entityCount == 10);

    s.area = PlotArea::Extents;
    auto t0 = std::chrono::steady_clock::now();
    auto sheet = buildSheet(doc, s).value();
    auto pdf = pdfBytes(sheet, true);
    std::string svg = svgString(sheet);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(pdf.ok());
    CHECK(sheet.entityCount == 20000);
    CHECK(sec < 2.0);
}
