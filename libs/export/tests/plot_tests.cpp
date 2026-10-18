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
#include "tbcad/blocks/display.h"
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

TEST_CASE("a plot window clips the sheet to the window") {
    Document doc;
    addEntity(doc, line({0, 0}, {100, 100}));
    PlotSettings s;
    s.area = PlotArea::Window;
    s.window.min = {0, 0};
    s.window.max = {50, 50};
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    const auto& prims = sheet.value().prims;
    REQUIRE(prims.size() >= 3);
    const auto* begin = std::get_if<PlotClip>(&prims.front());
    const auto* end = std::get_if<PlotClip>(&prims.back());
    REQUIRE(begin);
    REQUIRE(end);
    CHECK(begin->begin);
    CHECK_FALSE(end->begin);
    CHECK(begin->rect.width() == doctest::Approx(277));

    s.area = PlotArea::Extents;  // the whole drawing fits: nothing to clip
    auto whole = buildSheet(doc, s);
    REQUIRE(whole.ok());
    CHECK_FALSE(std::get_if<PlotClip>(&whole.value().prims.front()));
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
    // A true colour whose nearest palette entry has no override is left alone.
    CHECK(paths[2]->style.color == Rgba{10, 200, 30, 255});
    // Monochrome mode still wins over the table.
    s.color = ColorMode::Monochrome;
    auto mono = buildSheet(doc, s);
    for (const auto& p : mono.value().prims)
        if (const auto* pp = std::get_if<PlotPath>(&p))
            CHECK(pp->style.color == Rgba{0, 0, 0, 255});
    // The built-in monochrome table makes every colour black, true colours included.
    s.color = ColorMode::AsDisplayed;
    s.styleTable =
        std::make_shared<plotstyle::PlotStyleTable>(*plotstyle::builtinTable("monochrome.ctb"));
    auto bw = buildSheet(doc, s);
    int black = 0;
    for (const auto& p : bw.value().prims)
        if (const auto* pp = std::get_if<PlotPath>(&p))
            black += pp->style.color == Rgba{0, 0, 0, 255};
    CHECK(black == 3);  // the true colour follows its nearest palette entry and goes black too
}

TEST_CASE("a transparent object prints as a lighter colour, also through a black pen") {
    Document doc;
    Entity half = line({0, 0}, {100, 0});
    half.common.color = Color::index(1);
    half.common.transparency = 50;
    addEntity(doc, half);

    PlotSettings s;
    auto firstPath = [&](const PlotSettings& ps) {
        auto sheet = buildSheet(doc, ps);
        REQUIRE(sheet.ok());
        for (const auto& p : sheet.value().prims)
            if (const auto* pp = std::get_if<PlotPath>(&p))
                return pp->style.color;
        FAIL("no path");
        return Rgba{};
    };
    const Rgba drawn = paletteColor(1);
    const Rgba light = firstPath(s);
    CHECK(light.g > drawn.g);  // mixed halfway with the white sheet
    CHECK(light.b > drawn.b);
    s.transparency = false;
    CHECK(firstPath(s) == drawn);

    // A black pen at 50 % transparency prints grey.
    plotstyle::PlotStyleTable t;
    t.entries[1].color = Rgba{0, 0, 0, 255};
    s.transparency = true;
    s.styleTable = std::make_shared<plotstyle::PlotStyleTable>(t);
    const Rgba grey = firstPath(s);
    CHECK(grey.r == grey.g);
    CHECK(grey.r == grey.b);
    CHECK(grey.r > 100);
    CHECK(grey.r < 160);
}

TEST_CASE("a black pen at 73 percent transparency prints as the solid grey 186") {
    Document doc;
    Entity e = line({0, 0}, {100, 0});
    e.common.color = Color::index(8);
    e.common.transparency = 73;
    addEntity(doc, e);

    plotstyle::PlotStyleTable t;
    t.entries[8].color = Rgba{0, 0, 0, 255};
    PlotSettings s;
    s.styleTable = std::make_shared<plotstyle::PlotStyleTable>(t);
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    for (const auto& p : sheet.value().prims)
        if (const auto* pp = std::get_if<PlotPath>(&p)) {
            CHECK(pp->style.color.a == 255);
            CHECK(std::abs(int(pp->style.color.r) - 186) <= 1);
            CHECK(pp->style.color.r == pp->style.color.g);
            CHECK(pp->style.color.r == pp->style.color.b);
            return;
        }
    FAIL("no path");
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

TEST_CASE("a picture block is plotted as a picture in PDF, SVG and raster output") {
    namespace fs = std::filesystem;
    blocks::registerInsertDisplay();
    const fs::path dir = fs::temp_directory_path() / "tbcad_plot_picture";
    fs::create_directories(dir);
    {
        // 4 x 2 pixels, solid red.
        std::vector<std::uint8_t> px;
        for (int i = 0; i < 8; ++i)
            px.insert(px.end(), {255, 0, 0, 255});
        std::size_t n = 0;
        void* png = tdefl_write_image_to_png_file_in_memory_ex(px.data(), 4, 2, 4, &n, 6, MZ_FALSE);
        REQUIRE(png);
        std::ofstream f(dir / "red.png", std::ios::binary);
        f.write(static_cast<const char*>(png), static_cast<std::streamsize>(n));
        mz_free(png);
    }
    Document doc;
    {
        auto tx = doc.begin("picture");
        BlockId b = tx.addBlock(BlockDef{.name = "red", .base = {0, 0}}).value();
        PolylineData frame;
        frame.closed = true;
        for (Vec2 v : {Vec2{0, 0}, Vec2{4, 0}, Vec2{4, 2}, Vec2{0, 2}})
            frame.v.push_back({v, 0, 0, 0});
        Entity fe;
        fe.data = std::move(frame);
        REQUIRE(tx.add(std::move(fe), b).ok());
        REQUIRE(tx.modifyBlock(b, [](BlockDef& d) { d.xrefPath = "red.png"; }).ok());
        Entity ie;
        ie.data = Indirect<InsertData>(InsertData{b, {0, 0}, {25, 25}, 0});  // 100 x 50 mm
        REQUIRE(tx.add(std::move(ie)).ok());
        tx.commit();
    }
    PlotSettings s;
    s.paperName = "A4";
    s.hostFile = dir / "drawing.tbcad";
    auto sheet = buildSheet(doc, s);
    REQUIRE_MESSAGE(sheet.ok(), (sheet.ok() ? "" : sheet.error().message));
    const PlotImage* pic = nullptr;
    std::size_t paths = 0;
    for (const PlotPrimitive& p : sheet.value().prims) {
        if (const auto* i = std::get_if<PlotImage>(&p))
            pic = i;
        else if (std::get_if<PlotPath>(&p))
            ++paths;
    }
    REQUIRE(pic);
    CHECK(paths == 0);  // the frame is not plotted
    CHECK(pic->data->width == 4);
    const Vec2 bl = pic->corners[0], tr = pic->corners[2];
    CHECK(tr.x - bl.x > 10);
    CHECK(tr.y - bl.y > 5);

    auto pdf = pdfBytes(sheet.value(), false);
    REQUIRE(pdf.ok());
    CHECK(pdf.value().find("/Subtype /Image") != std::string::npos);
    CHECK(pdf.value().find("/Im0 Do") != std::string::npos);
    CHECK(svgString(sheet.value()).find("<image ") != std::string::npos);

    auto ras = rasterize(sheet.value(), 50, 100'000'000);
    REQUIRE(ras.ok());
    const RasterImage& r = ras.value();
    const double k = 50 / 25.4;
    const int cx = static_cast<int>(((bl.x + tr.x) / 2) * k);
    const int cy = r.height - static_cast<int>(((bl.y + tr.y) / 2) * k);
    const std::uint8_t* c = r.rgb.data() + (static_cast<std::size_t>(cy) * r.width + cx) * 3;
    CHECK(c[0] == 255);
    CHECK(c[1] < 10);
    CHECK(c[2] < 10);

    // A picture whose file cannot be found is left out.
    s.hostFile = dir / "nowhere" / "drawing.tbcad";
    auto missing = buildSheet(doc, s);
    REQUIRE(missing.ok());
    for (const PlotPrimitive& p : missing.value().prims)
        CHECK_FALSE(std::get_if<PlotImage>(&p));
    fs::remove_all(dir);
}

TEST_CASE("a table that keeps grey object colours leaves a layer of index colour 253 grey") {
    Document doc;
    Entity e = line({0, 0}, {100, 0});
    e.common.color = Color::index(253);
    addEntity(doc, e);
    // Like the office table: every colour black except the greys, which keep the object colour.
    plotstyle::PlotStyleTable t;
    for (int i = 1; i <= 249; ++i)
        t.entries[static_cast<std::size_t>(i)].color = Rgba{0, 0, 0, 255};
    PlotSettings s;
    s.styleTable = std::make_shared<plotstyle::PlotStyleTable>(t);
    s.traceColors = true;
    auto sheet = buildSheet(doc, s);
    REQUIRE(sheet.ok());
    bool found = false;
    for (const auto& p : sheet.value().prims)
        if (const auto* pp = std::get_if<PlotPath>(&p)) {
            CHECK(pp->style.color == Rgba{130, 130, 130, 255});
            found = true;
        }
    CHECK(found);
    REQUIRE(sheet.value().trace.size() == 1);
    CHECK(sheet.value().trace[0].find("ACI 253") != std::string::npos);
    CHECK(sheet.value().trace[0].find("130,130,130, 1 uses") != std::string::npos);
}

TEST_CASE("a PDF has one optional-content group per layer, bound xref layers named with a bar") {
    Document doc;
    {
        auto tx = doc.begin("layers");
        Layer a;
        a.name = "WALLS";
        Layer b;
        b.name = "x_col_03$0$S-COL";
        const LayerId la = tx.addLayer(a).value();
        const LayerId lb = tx.addLayer(b).value();
        Entity e1 = line({0, 0}, {50, 0});
        e1.common.layer = la;
        Entity e2 = line({0, 5}, {50, 5});
        e2.common.layer = lb;
        Entity e3 = line({0, 9}, {50, 9});
        e3.common.layer = la;
        REQUIRE(tx.add(e1).ok());
        REQUIRE(tx.add(e2).ok());
        REQUIRE(tx.add(e3).ok());
        tx.commit();
    }
    PlotSettings s;
    auto sheet = buildSheet(doc, s).value();
    REQUIRE(sheet.layerNames.size() == 2);
    auto pdf = pdfBytes(sheet);
    REQUIRE(pdf.ok());
    const std::string& t = pdf.value();
    CHECK(t.rfind("%PDF-1.5", 0) == 0);
    CHECK(t.find("/Type /OCG /Name (WALLS)") != std::string::npos);
    CHECK(t.find("/Type /OCG /Name (x_col_03|S-COL)") != std::string::npos);
    CHECK(t.find("/OCProperties") != std::string::npos);
    std::size_t bdc = 0, emc = 0;
    for (std::size_t at = 0; (at = t.find(" BDC", at)) != std::string::npos; ++at)
        ++bdc;
    for (std::size_t at = 0; (at = t.find("\nEMC\n", at)) != std::string::npos; ++at)
        ++emc;
    CHECK(bdc == 3);  // WALLS, xref layer, WALLS again
    CHECK(emc == 3);
}
