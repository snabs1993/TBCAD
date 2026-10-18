// Text at scale and across fonts: 20,000 texts in four kinds of font (bundled Thai, TBCAD stroke,
// a TrueType file, an SHX file) - first frame, zoom and style change; and the plot of Thai text
// has the same shape as the screen's.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "tbcad/cmd/register_all.h"
#include "tbcad/export/plot.h"
#include "tbcad/geom/geom.h"
#include "tbcad/text/text.h"
#include "tbcad/ui/app_core.h"

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace tbcad;

namespace {

double ms(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// A compiled shapes font with an 'L' and an 'A'.
std::vector<unsigned char> tinyShx() {
    std::vector<unsigned char> out;
    const std::string head = "AutoCAD-86 shapes 1.0\r\n\x1a";
    out.insert(out.end(), head.begin(), head.end());
    auto u16 = [&](unsigned x) {
        out.push_back(x & 255);
        out.push_back(x >> 8);
    };
    const std::vector<unsigned char> info = {'T', 0, 10, 3, 0, 0};
    const std::vector<unsigned char> el = {'L',  0, 2, 8, 0, 10, 1, 8, 0,
                                           0xF6, 8, 6, 0, 2, 8,  2, 0, 0};
    u16(0);
    u16(255);
    u16(2);
    u16(0);
    u16(info.size());
    u16('L');
    u16(el.size());
    out.insert(out.end(), info.begin(), info.end());
    out.insert(out.end(), el.begin(), el.end());
    return out;
}

}  // namespace

TEST_CASE("perf: 20,000 texts in mixed fonts - first frame, zoom, style change") {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* env = std::getenv("TBCAD_BENCH_N");
    const int n = env ? std::atoi(env) : 20000;
    const auto dir =
        std::filesystem::temp_directory_path() / ("tbcad_text_fonts_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "shx");
    {
        std::ofstream f(dir / "shx" / "tiny.shx", std::ios::binary);
        const auto b = tinyShx();
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    }
    text::setFontSearchPaths({dir / "shx"});

    cmd::Registry registry;
    registerAllCommands(registry);
    ui::AppCore core(registry, platform::AppPaths{dir / "cfg", dir / "data"});
    core.setViewportSize(1400, 900);
    Document& doc = core.document();
    {
        auto tx = doc.begin("fill");
        TextStyleId styles[4];
        TextStyle a;
        a.name = "ThaiDefault";
        styles[0] = tx.addTextStyle(a).value();
        TextStyle b;
        b.name = "Stroke";
        b.fontFamily = "TBCAD Stroke";
        styles[1] = tx.addTextStyle(b).value();
        TextStyle c;
        c.name = "Lato";
        c.fontFile = (std::filesystem::path(TBCAD_FONTS_DIR) / "Lato-Regular.ttf").string();
        c.widthFactor = 0.85;
        c.oblique = 0.2;
        styles[2] = tx.addTextStyle(c).value();
        TextStyle d;
        d.name = "Shx";
        d.shxFile = "tiny.shx";
        d.fontFamily = "TBCAD Stroke";
        styles[3] = tx.addTextStyle(d).value();
        const char* words[] = {"ห้องนอน 1", "Room 12", "LLL", "ครัว / Kitchen", "ชั้น 2 FL+2.40"};
        for (int i = 0; i < n; ++i) {
            Entity e;
            TextData t;
            t.position = {(i % 200) * 900.0, (i / 200) * 400.0};
            t.height = 250;
            t.utf8 = words[i % 5];
            t.style = styles[i % 4];
            e.data = std::move(t);
            (void)tx.add(std::move(e));
        }
        tx.commit();
    }
    auto t = std::chrono::steady_clock::now();
    core.zoomExtents();
    core.updateFrame();
    const double first = ms(t);
    t = std::chrono::steady_clock::now();
    core.zoomToBox(BBox2{{0, 0}, {40000, 12000}});
    core.updateFrame();
    const double zoom = ms(t);
    t = std::chrono::steady_clock::now();
    core.submitText("STYLE");
    core.submitText("Lato");
    core.submitText("Sarabun");
    core.submitText("1");
    core.submitText("0");
    core.submitText("0");
    core.zoomExtents();
    core.updateFrame();
    const double restyle = ms(t);
    std::printf("%d texts: first frame %.0f ms, zoom window %.0f ms, restyle all %.0f ms\n", n,
                first, zoom, restyle);
    CHECK(first < 20000);
    CHECK(restyle < 20000);
    text::setFontSearchPaths({});
    std::filesystem::remove_all(dir);
}

TEST_CASE("Thai text has the same outline on the plot as on the screen") {
    Document doc;
    Entity e;
    TextData t;
    t.position = {100, 100};
    t.height = 10;
    t.utf8 = "ห้องนอนใหญ่ ก้านกล้วย";
    t.style = kTextStyleStandard;
    e.data = t;
    {
        auto tx = doc.begin("t");
        (void)tx.add(std::move(e));
        tx.commit();
    }
    text::registerDisplay();
    geom::DisplayContext ctx;
    ctx.pixelSizeMm = 0.005;
    DisplayList screen;
    doc.forEachEntity([&](const Entity& ent) { geom::buildDisplay(doc, ent, ctx, screen); });
    BBox2 sb;
    for (const auto& it : screen.items)
        if (const auto* f = std::get_if<DisplayFill>(&it))
            for (const auto& ring : f->rings)
                for (const Vec2& p : ring)
                    sb.expand(p);
    REQUIRE(!sb.empty());

    exportlib::PlotSettings ps;
    ps.fitToPaper = false;
    ps.scaleN = 1;
    ps.paperName = "A3";
    ps.centerOnPaper = false;
    ps.offsetMm = {0, 0};
    ps.marginMm = 0;
    auto sheet = exportlib::buildSheet(doc, ps);
    REQUIRE(sheet.ok());
    BBox2 pb;
    int fills = 0;
    for (const auto& prim : sheet.value().prims) {
        if (const auto* f = std::get_if<exportlib::PlotFill>(&prim)) {
            ++fills;
            for (const auto& ring : f->rings)
                for (const Vec2& p : ring)
                    pb.expand(p);
        }
    }
    REQUIRE(fills > 0);
    const Vec2 off = {pb.min.x - sb.min.x, pb.min.y - sb.min.y};  // paper = drawing + offset
    CHECK(pb.max.x - pb.min.x == doctest::Approx(sb.max.x - sb.min.x).epsilon(0.002));
    CHECK(pb.max.y - pb.min.y == doctest::Approx(sb.max.y - sb.min.y).epsilon(0.002));
    (void)off;
}
