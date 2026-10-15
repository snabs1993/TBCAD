#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "miniz.h"
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

bool requireQpdf() {
    std::string v;
#ifdef _WIN32
    std::size_t n = 0;
    char* buf = nullptr;
    if (_dupenv_s(&buf, &n, "TBCAD_REQUIRE_QPDF") == 0 && buf)
        v = buf;
    std::free(buf);
#else
    if (const char* e = std::getenv("TBCAD_REQUIRE_QPDF"))
        v = e;
#endif
    return !v.empty();
}

void checkWithQpdf(const PlotSheet& sheet, const char* name) {
    static const bool available =
        std::system((std::string("qpdf --version > ") + kNull + " 2>&1").c_str()) == 0;
    if (!available) {
        if (requireQpdf())
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

TEST_CASE("fixed scale larger than the paper reports overflow - offset places the drawing") {
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

TEST_CASE("monochrome turns red black - white is always black - greyscale") {
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

TEST_CASE("a plot style table maps colour, lineweight and screening per palette colour") {
    Document doc;
    Entity red = line({0, 0}, {100, 0});
    red.common.color = Color::index(1);
    red.common.lineweight = LineWeight::mm(0.25);
    addEntity(doc, red);
    Entity blue = line({0, 10}, {100, 10});
    blue.common.color = Color::index(5);
    addEntity(doc, blue);
    Entity truecolor = line({0, 20}, {100, 20});
    truecolor.common.color = Color::rgb(10, 200, 30);
    addEntity(doc, truecolor);

    plotstyle::PlotStyleTable t;
    t.entries[1].color = Rgba{0, 0, 0, 255};
    t.entries[1].lineweightMm = 0.7;
    t.entries[5].screen = 50;
    PlotSettings s;
    s.styleTable = std::make_shared<plotstyle::PlotStyleTable>(t);
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    std::vector<const PlotPath*> paths;
    for (const auto& p : sheet.value().prims)
        if (const auto* pp = std::get_if<PlotPath>(&p))
            paths.push_back(pp);
    REQUIRE(paths.size() == 3);
    // Red -> black, 0.7 mm.
    CHECK(paths[0]->style.color == Rgba{0, 0, 0, 255});
    CHECK(paths[0]->style.widthMm == doctest::Approx(0.7));
    // Blue screened to 50%: lighter than the drawn blue, lineweight untouched.
    const Rgba drawn = paletteColor(5);
    CHECK(paths[1]->style.color.r > drawn.r);
    CHECK(paths[1]->style.widthMm == doctest::Approx(0.25));
    // A true colour is left alone.
    CHECK(paths[2]->style.color == Rgba{10, 200, 30, 255});
    // Monochrome mode still wins over the table.
    s.color = ColorMode::Monochrome;
    auto mono = buildSheet(doc, s);
    for (const auto& p : mono.value().prims)
        if (const auto* pp = std::get_if<PlotPath>(&p))
            CHECK(pp->style.color == Rgba{0, 0, 0, 255});
    // The built-in monochrome table makes every palette colour black.
    s.color = ColorMode::AsDisplayed;
    s.styleTable =
        std::make_shared<plotstyle::PlotStyleTable>(*plotstyle::builtinTable("monochrome.ctb"));
    auto bw = buildSheet(doc, s);
    int black = 0;
    for (const auto& p : bw.value().prims)
        if (const auto* pp = std::get_if<PlotPath>(&p))
            black += pp->style.color == Rgba{0, 0, 0, 255};
    CHECK(black == 2);  // the two palette-coloured lines; the true colour stays
}

TEST_CASE("lineweights are absolute on paper - dashes scale with the plot scale") {
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
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        CHECK(ss.str() == svg);
    }
    std::filesystem::remove(path);
}

TEST_CASE("culling: entities outside a window are not plotted - 20,000 lines plot quickly") {
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

namespace {
std::uint32_t be32(const std::string& s, std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3]));
}
}  // namespace

TEST_CASE("raster: A4 at 150 dpi has the right size, white paper and an anti-aliased red line") {
    Document doc;
    Entity e = line({0, 0}, {100, 50});
    e.common.color = Color::index(1);  // red
    addEntity(doc, e);
    PlotSettings s;
    s.paperName = "A4";
    s.color = ColorMode::AsDisplayed;
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    auto img = rasterize(sheet.value(), 150);
    REQUIRE(img.ok());
    CHECK(img.value().width == 1754);  // landscape default
    CHECK(img.value().height == 1240);
    CHECK(img.value().rgb.size() == 1754u * 1240u * 3u);
    std::size_t white = 0, red = 0, partial = 0;
    for (std::size_t i = 0; i < img.value().rgb.size(); i += 3) {
        const auto r = img.value().rgb[i], g = img.value().rgb[i + 1], b = img.value().rgb[i + 2];
        if (r == 255 && g == 255 && b == 255)
            ++white;
        else if (r > 200 && g < 60 && b < 60)
            ++red;
        else if (r > g && g == b)
            ++partial;
    }
    CHECK(red > 100);
    CHECK(partial > 100);  // the edges are anti-aliased
    CHECK(white > img.value().rgb.size() / 3 * 95 / 100);

    CHECK_FALSE(rasterize(sheet.value(), 5).ok());
    CHECK_FALSE(rasterize(sheet.value(), 150, 1000).ok());
}

TEST_CASE("PNG: signature, dimensions, resolution and pixels survive a round trip") {
    Document doc;
    addEntity(doc, line({0, 0}, {100, 0}));
    PlotSettings s;
    s.paperName = "A4";
    auto sheet = buildSheet(doc, s).value();
    auto img = rasterize(sheet, 100).value();
    const std::string png = pngBytes(img);
    REQUIRE(png.size() > 60);
    CHECK(png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(be32(png, 16) == static_cast<std::uint32_t>(img.width));
    CHECK(be32(png, 20) == static_cast<std::uint32_t>(img.height));
    const auto phys = png.find("pHYs");
    REQUIRE(phys != std::string::npos);
    CHECK(be32(png, phys + 4) == 3937);  // 100 dpi in pixels per metre
    // Inflate and undo the Up filter.
    const auto idat = png.find("IDAT");
    REQUIRE(idat != std::string::npos);
    const std::uint32_t zlen = be32(png, idat - 4);
    const std::size_t rowBytes = static_cast<std::size_t>(img.width) * 3 + 1;
    std::string raw(rowBytes * static_cast<std::size_t>(img.height), '\0');
    mz_ulong rawLen = static_cast<mz_ulong>(raw.size());
    REQUIRE(mz_uncompress(reinterpret_cast<unsigned char*>(raw.data()), &rawLen,
                          reinterpret_cast<const unsigned char*>(png.data()) + idat + 4,
                          zlen) == MZ_OK);
    CHECK(rawLen == raw.size());
    CHECK(png.substr(png.size() - 8, 4) == "IEND");
    std::vector<std::uint8_t> back(img.rgb.size());
    for (int y = 0; y < img.height; ++y)
        for (std::size_t i = 0; i < static_cast<std::size_t>(img.width) * 3; ++i) {
            const auto up = y ? back[(static_cast<std::size_t>(y) - 1) * (rowBytes - 1) + i] : 0;
            back[static_cast<std::size_t>(y) * (rowBytes - 1) + i] = static_cast<std::uint8_t>(
                static_cast<unsigned char>(raw[static_cast<std::size_t>(y) * rowBytes + 1 + i]) +
                up);
        }
    CHECK(back == img.rgb);
}

TEST_CASE("JPEG: markers, dimensions and density - files are written") {
    Document doc;
    addEntity(doc, line({0, 0}, {100, 0}));
    PlotSettings s;
    s.paperName = "A4";
    auto sheet = buildSheet(doc, s).value();
    auto img = rasterize(sheet, 100).value();
    const std::string jpg = jpegBytes(img, 85);
    REQUIRE(jpg.size() > 200);
    CHECK(static_cast<unsigned char>(jpg[0]) == 0xFF);
    CHECK(static_cast<unsigned char>(jpg[1]) == 0xD8);
    CHECK(static_cast<unsigned char>(jpg[jpg.size() - 2]) == 0xFF);
    CHECK(static_cast<unsigned char>(jpg[jpg.size() - 1]) == 0xD9);
    const auto sof = jpg.find("\xFF\xC0");
    REQUIRE(sof != std::string::npos);
    auto be16 = [&](std::size_t at) {
        return static_cast<int>((static_cast<unsigned char>(jpg[at]) << 8) |
                                static_cast<unsigned char>(jpg[at + 1]));
    };
    CHECK(be16(sof + 5) == img.height);
    CHECK(be16(sof + 7) == img.width);
    // Whiter paper compresses far smaller than the raw pixels.
    CHECK(jpg.size() < img.rgb.size() / 10);

    const auto dir = std::filesystem::temp_directory_path() / "tbcad_raster_test";
    std::filesystem::create_directories(dir);
    CHECK(writePng(sheet, (dir / "a.png").string(), 72).ok());
    CHECK(writeJpeg(sheet, (dir / "a.jpg").string(), 72, 80).ok());
    CHECK(std::filesystem::file_size(dir / "a.png") > 100);
    CHECK(std::filesystem::file_size(dir / "a.jpg") > 100);
    std::filesystem::remove_all(dir);
}
