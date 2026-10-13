#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/layout/layout.h"
#include "tbcad/plot/plot.h"
#include "tbcad/ui/ui_registry.h"

using namespace tbcad;

namespace {

long uniqueId() {
    static long counter = 0;
    return static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count() %
                             1000000007L) *
               16 +
           (counter++ & 15);
}

struct Fixture {
    Document doc;
    cmd::Registry reg;
    std::unique_ptr<cmd::ScriptHost> host;
    // ctest runs the test cases in parallel processes: every fixture gets a folder of its own.
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("tbcad_plot_cmd_tests_" + std::to_string(uniqueId()));

    Fixture() {
        std::filesystem::create_directories(dir);
        plot::registerCommands(reg);
        auto tx = doc.begin("line");
        Entity e;
        e.data = LineData{{0, 0}, {10000, 0}};
        REQUIRE(tx.add(e).ok());
        tx.commit();
        host = std::make_unique<cmd::ScriptHost>(doc, reg);
    }
    ~Fixture() { std::filesystem::remove_all(dir); }
    std::string path(const char* name) const { return (dir / name).string(); }
};

std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("commands are registered with their aliases") {
    Fixture f;
    CHECK(f.reg.find("PLOT") != nullptr);
    CHECK(f.reg.find("PRINT") == f.reg.find("PLOT"));
    CHECK(f.reg.find("EXPORTPDF") != nullptr);
    CHECK(f.reg.find("EXPORTSVG") != nullptr);
}

TEST_CASE("EXPORTPDF writes a PDF with the chosen paper and scale") {
    Fixture f;
    auto out = f.path("a3.pdf");
    auto r = f.host->run("EXPORTPDF", {"Scale", "1:100", "", out});
    CHECK(r.finished);
    REQUIRE(std::filesystem::exists(out));
    std::string pdf = slurp(out);
    CHECK(pdf.rfind("%PDF-", 0) == 0);
    CHECK(f.host->logContains("Plotted 1 objects"));
    CHECK(f.host->logContains("scale 1:100"));
    // Settings stick for the next run.
    auto s = plot::loadSettings(*f.host);
    CHECK_FALSE(s.fitToPaper);
    CHECK(s.scaleN == doctest::Approx(100));
    CHECK(plot::lastFile(*f.host, plot::Format::Pdf) == out);
}

TEST_CASE("EXPORTSVG: orientation, paper and colour options") {
    Fixture f;
    auto out = f.path("a4.svg");
    auto r = f.host->run(
        "EXPORTSVG", {"Paper", "a4", "Orientation", "Portrait", "Color", "Monochrome", "", out});
    CHECK(r.finished);
    std::string svg = slurp(out);
    CHECK(svg.find("width=\"210mm\"") != std::string::npos);
    CHECK(svg.find("height=\"297mm\"") != std::string::npos);
    auto s = plot::loadSettings(*f.host);
    CHECK(s.paperName == "A4");
    CHECK_FALSE(s.landscape);
    CHECK(s.color == exportlib::ColorMode::Monochrome);
}

TEST_CASE("an extension is added; Esc leaves nothing behind") {
    Fixture f;
    auto base = f.path("noext");
    auto r = f.host->run("PLOT", {"", base});
    CHECK(r.finished);
    CHECK(std::filesystem::exists(base + ".pdf"));

    auto out = f.path("cancel.pdf");
    auto c = f.host->run("PLOT", {"", cmd::ScriptItem::esc()});
    CHECK(c.finished);
    CHECK_FALSE(std::filesystem::exists(f.path("cancel.pdf")));
    CHECK_FALSE(f.host->undoStack().canUndo());  // plotting never touches the drawing
}

TEST_CASE("bad paper and bad scale are refused with a message") {
    Fixture f;
    auto r = f.host->run("EXPORTPDF", {"Paper", "B7", "Scale", "banana", "Esc"});
    CHECK(f.host->logContains("Unknown paper size"));
    CHECK(f.host->logContains("Not a scale"));
    CHECK(plot::loadSettings(*f.host).paperName == "A3");
}

TEST_CASE("window area uses two corners; an empty drawing reports an error") {
    Fixture f;
    auto out = f.path("win.pdf");
    auto r = f.host->run("EXPORTPDF", {"Area", "Window", "0,0", "5000,100", "", out});
    CHECK(r.finished);
    CHECK(f.host->logContains("Plotted 1 objects"));
    auto s = plot::loadSettings(*f.host);
    CHECK(s.area == exportlib::PlotArea::Window);
    CHECK(s.window.max.x == doctest::Approx(5000));

    Document empty;
    cmd::ScriptHost h2(empty, f.reg);
    auto r2 = h2.run("EXPORTPDF",
                     {"", f.path("none.pdf"), cmd::ScriptItem::esc(), cmd::ScriptItem::esc()});
    CHECK(h2.logContains("Nothing to plot"));
    CHECK_FALSE(std::filesystem::exists(f.path("none.pdf")));
}

TEST_CASE("parseScale and withExtension") {
    exportlib::PlotSettings s;
    CHECK(plot::parseScale("1:50", s));
    CHECK(s.scaleN == 50);
    CHECK_FALSE(s.fitToPaper);
    CHECK(plot::parseScale("200", s));
    CHECK(s.scaleN == 200);
    CHECK(plot::parseScale("Fit", s));
    CHECK(s.fitToPaper);
    CHECK_FALSE(plot::parseScale("2:1", s));
    CHECK_FALSE(plot::parseScale("0", s));
    CHECK(plot::withExtension("a", plot::Format::Pdf) == "a.pdf");
    CHECK(plot::withExtension("a.PDF", plot::Format::Pdf) == "a.PDF");
    CHECK(plot::withExtension("dir.x/a", plot::Format::Svg) == "dir.x/a.svg");
}

TEST_CASE("registerUi adds the File menu entries") {
    ui::UiRegistry ui;
    plot::registerUi(ui);
    bool plotItem = false;
    for (const auto& m : ui.menuItems())
        plotItem |= m.command == "PLOT";
    CHECK(plotItem);
}

namespace {
struct DocHost : cmd::ScriptHost {
    using cmd::ScriptHost::ScriptHost;
    std::string file{};
    std::vector<std::string> panels{};
    std::string documentFile() const override { return file; }
    void openPanel(std::string_view id) override { panels.emplace_back(id); }
};
}  // namespace

TEST_CASE("the default file name follows the open drawing; PLOTPANEL opens the panel") {
    Fixture f;
    DocHost h(f.doc, f.reg);
    CHECK(plot::defaultFile(h, plot::Format::Pdf) == "drawing.pdf");
    h.file = "C:\\work\\plan.v2.tbcad";
    CHECK(plot::defaultFile(h, plot::Format::Pdf) == "C:\\work\\plan.v2.pdf");
    CHECK(plot::defaultFile(h, plot::Format::Svg) == "C:\\work\\plan.v2.svg");
    h.file = "/tmp/dir.x/plan";
    CHECK(plot::defaultFile(h, plot::Format::Pdf) == "/tmp/dir.x/plan.pdf");

    auto r = h.run("PLOTPANEL", {});
    CHECK(r.finished);
    REQUIRE(h.panels.size() == 1);
    CHECK(h.panels[0] == "plot");
}

TEST_CASE("the Layout option plots a layout at 1:1 instead of model space") {
    Fixture f;
    {
        auto tx = f.doc.begin("layout");
        REQUIRE(layout::createLayout(tx, "Plan", "A3").ok());
        REQUIRE(layout::addViewport(tx, "Plan", {{10, 10}, {410, 287}}, {5000, 0}, 100).ok());
        tx.commit();
    }
    auto out = f.path("layout.pdf");
    auto r = f.host->run("EXPORTPDF", {"Layout", "Plan", "", out});
    CHECK(r.finished);
    REQUIRE(std::filesystem::exists(out));
    CHECK(f.host->logContains("Layout Plan: Plotted"));
    CHECK(plot::plotLayout(*f.host) == "Plan");
    // Unknown layout: an error message, no file.
    auto bad = f.path("bad.pdf");
    f.host->run("EXPORTPDF", {"Layout", "Nope", "", bad});
    CHECK_FALSE(std::filesystem::exists(bad));
}

TEST_CASE("PLOTSTYLE selects a table; the default is monochrome and None plots as drawn") {
    Fixture f;
    {
        auto tx = f.doc.begin("red");
        Entity e;
        e.data = LineData{{0, 50}, {10000, 50}};
        e.common.color = Color::index(1);
        REQUIRE(tx.add(e).ok());
        tx.commit();
    }
    CHECK(plot::plotStyleName(*f.host) == "monochrome.ctb");
    auto mono = f.path("mono.svg");
    f.host->run("EXPORTSVG", {"", mono});
    REQUIRE(std::filesystem::exists(mono));
    auto svgMono = slurp(mono);
    CHECK(svgMono.find("#ff0000") == std::string::npos);

    f.host->run("PLOTSTYLE", {"Select", "none"});
    CHECK(plot::plotStyleName(*f.host) == "none");
    auto drawn = f.path("drawn.svg");
    f.host->run("EXPORTSVG", {"", drawn});
    CHECK(slurp(drawn).find("#ff0000") != std::string::npos);

    f.host->run("PLOTSTYLE", {"Select", "grayscale.ctb"});
    CHECK(plot::plotStyleName(*f.host) == "grayscale.ctb");
    f.host->run("PLOTSTYLE", {"Select", "missing-table.ctb"});
    CHECK(f.host->logContains("missing-table.ctb"));
    CHECK(plot::plotStyleName(*f.host) == "grayscale.ctb");  // unchanged
    f.host->run("PLOTSTYLE", {"List"});
    CHECK(f.host->logContains("* grayscale.ctb"));
    CHECK(f.host->logContains("  monochrome.ctb"));
}

TEST_CASE("EXPORTPNG and EXPORTJPG write images at the chosen resolution") {
    Fixture f;
    CHECK(f.reg.find("EXPORTPNG") != nullptr);
    CHECK(f.reg.find("EXPORTJPEG") == f.reg.find("EXPORTJPG"));
    auto png = f.path("a4.png");
    auto r = f.host->run("EXPORTPNG", {"Paper", "A4", "Resolution", "50", "", png});
    CHECK(r.finished);
    std::string bytes = slurp(png);
    REQUIRE(bytes.size() > 50);
    CHECK(bytes.compare(1, 3, "PNG") == 0);
    CHECK(f.host->logContains("50 dpi"));
    CHECK(plot::plotDpi(*f.host) == doctest::Approx(50));
    // PNG header width / height: A4 landscape at 50 dpi is 585 x 413.
    auto be32 = [&](std::size_t at) {
        return (static_cast<unsigned>(static_cast<unsigned char>(bytes[at])) << 24) |
               (static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
               (static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
               static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 3]));
    };
    CHECK(be32(16) == 585u);
    CHECK(be32(20) == 413u);

    auto jpg = f.path("a4.jpg");
    r = f.host->run("EXPORTJPG", {"", jpg});
    CHECK(r.finished);
    bytes = slurp(jpg);
    REQUIRE(bytes.size() > 50);
    CHECK(static_cast<unsigned char>(bytes[0]) == 0xFF);
    CHECK(static_cast<unsigned char>(bytes[1]) == 0xD8);

    // Absurd resolutions are refused, not attempted.
    auto bad =
        f.host->run("EXPORTPNG", {"Resolution", "100000", "Resolution", "50", "", f.path("x.png")});
    CHECK(bad.finished);
}

TEST_CASE("forFormat swaps an export extension; dpi and quality have defaults") {
    CHECK(plot::forFormat("a.pdf", plot::Format::Png) == "a.png");
    CHECK(plot::forFormat("dir.x/a", plot::Format::Jpeg) == "dir.x/a.jpg");
    CHECK(plot::forFormat("a.JPEG", plot::Format::Svg) == "a.svg");
    CHECK(plot::forFormat("a.txt", plot::Format::Pdf) == "a.txt");
    Fixture f;
    CHECK(plot::plotDpi(*f.host) == doctest::Approx(300));
    CHECK(plot::jpegQuality(*f.host) == 90);
}
