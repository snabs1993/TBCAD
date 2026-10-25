#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/plot/plot.h"
#include "tbcad/text/text.h"
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
    CHECK(f.reg.find("-PLOT") == f.reg.find("PLOT"));
    CHECK(f.reg.find("-PUBLISH") == f.reg.find("PLOTSET"));
    CHECK(f.reg.find("PUBLISH") != f.reg.find("PLOTSET"));
    CHECK(f.reg.find("SHEETSET") == f.reg.find("PUBLISH"));
    CHECK(f.reg.find("SSM") == f.reg.find("PUBLISH"));
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

TEST_CASE("PLOT: the UpsideDown option is stored and turns the SVG") {
    Fixture f;
    auto out = f.path("up.svg");
    auto r = f.host->run("EXPORTSVG", {"UpsideDown", "Yes", "", out});
    CHECK(r.finished);
    CHECK(plot::loadSettings(*f.host).upsideDown);
    r = f.host->run("EXPORTSVG", {"UpsideDown", "No", "", f.path("up2.svg")});
    CHECK_FALSE(plot::loadSettings(*f.host).upsideDown);
}

TEST_CASE("PLOT Area View: a named view becomes the plot window") {
    Fixture f;
    {
        auto tx = f.doc.begin("view");
        REQUIRE(tx.modifyHeader([](Header& h) {
                      h.custom["tbcad.view.Detail"] = "500 20 1 400 200";
                  }).ok());
        tx.commit();
    }
    auto r = f.host->run("EXPORTSVG", {"Area", "View", "detail", "", f.path("v.svg")});
    CHECK(r.finished);
    auto s = plot::loadSettings(*f.host);
    CHECK(s.area == exportlib::PlotArea::Window);
    CHECK(s.window.min.x == doctest::Approx(300));
    CHECK(s.window.max.y == doctest::Approx(120));
    r = f.host->run("EXPORTSVG", {"Area", "View", "nope", "", f.path("v2.svg")});
    CHECK(f.host->logContains("There is no view named"));
}

TEST_CASE("an extension is added - Esc leaves nothing behind") {
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

TEST_CASE("window area uses two corners - an empty drawing reports an error") {
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
        plotItem |= m.command == "PLOTPANEL";
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

TEST_CASE("the default file name follows the open drawing - PLOTPANEL opens the panel") {
    Fixture f;
    DocHost h(f.doc, f.reg);
    // The plot goes to the plots folder, never next to the (maybe read-only) drawing; an unsaved
    // drawing is no exception.
    h.setSetting("plot.defaultFolder", "/home/u/Documents/TBCAD plots");
    CHECK(plot::defaultFile(h, plot::Format::Pdf) ==
          "/home/u/Documents/TBCAD plots/drawing-plot.pdf");
    h.file = "C:\\work\\plan.v2.tbcad";
    CHECK(plot::defaultFile(h, plot::Format::Pdf) ==
          "/home/u/Documents/TBCAD plots/plan.v2-plot.pdf");
    CHECK(plot::defaultFile(h, plot::Format::Svg) ==
          "/home/u/Documents/TBCAD plots/plan.v2-plot.svg");
    h.file = "/tmp/dir.x/plan";
    CHECK(plot::defaultFile(h, plot::Format::Pdf) == "/home/u/Documents/TBCAD plots/plan-plot.pdf");
    // After a plot, the folder of the last file is kept.
    plot::setLastFile(h, plot::Format::Pdf, "D:\\out\\old.pdf");
    CHECK(plot::defaultFile(h, plot::Format::Pdf) == "D:\\out\\plan-plot.pdf");
    // A last plot that went into the drawing's own folder is not reused.
    plot::setLastFile(h, plot::Format::Pdf, "/TMP/dir.x/old.pdf");
    CHECK(plot::defaultFile(h, plot::Format::Pdf) == "/home/u/Documents/TBCAD plots/plan-plot.pdf");

    auto r = h.run("PLOTPANEL", {});
    CHECK(r.finished);
    REQUIRE(h.panels.size() == 1);
    CHECK(h.panels[0] == "plot");
    CHECK(h.run("PREVIEW", {}).finished);
    CHECK(h.run("PAGESETUPPANEL", {}).finished);
    CHECK(h.panels == std::vector<std::string>{"plot", "plotpreview", "pagesetup"});
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

TEST_CASE(
    "the plot summary names the sheet of the layout being plotted, not the page setup paper") {
    Fixture f;
    {
        auto tx = f.doc.begin("layout");
        REQUIRE(layout::createLayout(tx, "Big", "A1").ok());
        tx.commit();
    }
    plot::setPlotLayout(*f.host, "Big");
    f.host->run("PLOT", {cmd::ScriptItem::esc()});
    REQUIRE_FALSE(f.host->prompts().empty());
    CHECK(f.host->prompts().front().find("sheet \"Big\" 841 x 594 mm") != std::string::npos);
}

TEST_CASE("PLOTSTYLE selects a table - the default is monochrome and None plots as drawn") {
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

TEST_CASE("forFormat swaps an export extension - dpi and quality have defaults") {
    CHECK(plot::forFormat("a.pdf", plot::Format::Png) == "a.png");
    CHECK(plot::forFormat("dir.x/a", plot::Format::Jpeg) == "dir.x/a.jpg");
    CHECK(plot::forFormat("a.JPEG", plot::Format::Svg) == "a.svg");
    CHECK(plot::forFormat("a.txt", plot::Format::Pdf) == "a.txt");
    Fixture f;
    CHECK(plot::plotDpi(*f.host) == doctest::Approx(300));
    CHECK(plot::jpegQuality(*f.host) == 90);
}

TEST_CASE("PAGESETUP saves the plot options, applies them and assigns them to layouts") {
    Fixture f;
    {
        auto tx = f.doc.begin("layout");
        REQUIRE(layout::createLayout(tx, "Plan", "A4", true).ok());
        REQUIRE(layout::createLayout(tx, "Detail", "A3", true).ok());
        tx.commit();
    }
    // Plot options as the user has them now.
    CHECK(f.host
              ->run("EXPORTSVG", {"Paper", "A2", "Orientation", "Portrait", "Scale", "1:50",
                                  "Color", "Monochrome", "Style", "none", "", f.path("x.svg")})
              .finished);
    CHECK(f.host->run("PAGESETUP", {"Save", "Big sheet"}).finished);
    auto saved = layout::findPageSetup(f.doc, "big sheet");
    REQUIRE(saved);
    CHECK(saved->settings.paperName == "A2");
    CHECK_FALSE(saved->settings.landscape);
    CHECK(saved->settings.scaleN == doctest::Approx(50));
    CHECK(saved->settings.color == exportlib::ColorMode::Monochrome);
    CHECK(saved->styleName == "none");

    // Change the options, then apply the setup: they come back.
    CHECK(f.host->run("EXPORTSVG", {"Paper", "A4", "Scale", "Fit", "", f.path("y.svg")}).finished);
    CHECK(plot::loadSettings(*f.host).paperName == "A4");
    CHECK(f.host->run("PAGESETUP", {"Apply", "Big sheet"}).finished);
    auto s = plot::loadSettings(*f.host);
    CHECK(s.paperName == "A2");
    CHECK(s.scaleN == doctest::Approx(50));
    CHECK_FALSE(s.styleTable);

    // Assign to a layout: it takes the paper; choosing that layout for plotting applies the
    // options.
    CHECK(f.host->run("PAGESETUP", {"Assign", "Big sheet", "Detail"}).finished);
    auto detail = layout::findLayout(f.doc, "Detail");
    REQUIRE(detail);
    CHECK(detail->pageSetup == "Big sheet");
    CHECK(detail->paperName == "A2");
    CHECK_FALSE(detail->landscape);
    plot::setPlotLayout(*f.host, "");
    CHECK(f.host->run("EXPORTSVG", {"Layout", "Detail", "Export", f.path("z.svg")}).finished);
    CHECK(f.host->logContains("Page setup \"Big sheet\" applied"));

    // Model space can have one too; List shows who uses it.
    CHECK(f.host->run("PAGESETUP", {"Assign", "Big sheet", "Model"}).finished);
    CHECK(layout::modelPageSetup(f.doc) == "Big sheet");
    CHECK(f.host->run("PAGESETUP", {"List"}).finished);
    CHECK(f.host->logContains("[model, Detail]"));

    CHECK(f.host->run("PAGESETUP", {"Rename", "Big sheet", "Poster"}).finished);
    CHECK(layout::findLayout(f.doc, "Detail")->pageSetup == "Poster");
    CHECK(f.host->run("PAGESETUP", {"Delete", "Poster"}).finished);
    CHECK(layout::pageSetups(f.doc).empty());
    CHECK(layout::findLayout(f.doc, "Detail")->pageSetup.empty());
    CHECK(f.host->run("PAGESETUP", {"Apply", "Nope"}).finished);
    CHECK(f.host->logContains("No page setup named \"Nope\""));
}

TEST_CASE("PSTYLEMODE and ASSIGNPSTYLE keep plot style names in the drawing") {
    Fixture f;
    CHECK(colorDependentPlotStyles(f.doc.header()));
    CHECK(f.host->run("PSTYLEMODE", {"Named"}).finished);
    CHECK_FALSE(colorDependentPlotStyles(f.doc.header()));
    CHECK(f.host->run("ASSIGNPSTYLE", {"Layer", "0", "Heavy"}).finished);
    CHECK(layerPlotStyle(f.doc.header(), kLayerZero) == "Heavy");
    const EntityId line = f.doc.modelSpace()[0];
    CHECK(resolvedPlotStyle(f.doc.header(), line, kLayerZero) == "Heavy");
    CHECK(f.host
              ->run("ASSIGNPSTYLE",
                    {"Objects", cmd::ScriptItem::pick(line), cmd::ScriptItem::enter(), "Thin"})
              .finished);
    CHECK(resolvedPlotStyle(f.doc.header(), line, kLayerZero) == "Thin");
    CHECK(f.host->run("ASSIGNPSTYLE", {"Layer", "Nope"}).finished);
    CHECK(f.host->logContains("No layer named \"Nope\""));
    CHECK(f.host->run("PSTYLEMODE", {"Colour"}).finished);
    CHECK(colorDependentPlotStyles(f.doc.header()));
}

TEST_CASE("PAGESETUP Import takes the page setups of another drawing") {
    Fixture f;
    CHECK(f.host->run("EXPORTSVG", {"Paper", "A2", "Scale", "1:50", "", f.path("x.svg")}).finished);
    CHECK(f.host->run("PAGESETUP", {"Save", "Big sheet"}).finished);
    REQUIRE(io::writeTbcad(f.doc, f.dir / "other.tbcad").ok());
    CHECK(f.host->run("PAGESETUP", {"Delete", "Big sheet"}).finished);
    REQUIRE(layout::pageSetups(f.doc).empty());
    CHECK(f.host->run("PAGESETUP", {"Import", f.path("other.tbcad")}).finished);
    auto got = layout::findPageSetup(f.doc, "Big sheet");
    REQUIRE(got);
    CHECK(got->settings.paperName == "A2");
    CHECK(f.host->run("PAGESETUP", {"Import", f.path("missing.tbcad")}).finished);
}

TEST_CASE("PAGESETUP Import reads the named page setups of a DXF file") {
    Fixture f;
    {
        std::ofstream dxf(f.dir / "setups.dxf");
        dxf << "  0\nSECTION\n  2\nHEADER\n  9\n$ACADVER\n  1\nAC1015\n  9\n$INSUNITS\n 70\n4\n"
               "  0\nENDSEC\n"
               "  0\nSECTION\n  2\nOBJECTS\n"
               "  0\nPLOTSETTINGS\n  5\n2A\n100\nAcDbPlotSettings\n  1\nSite plan\n"
               "  4\nISO_A1_(841.00_x_594.00_MM)\n  7\nmonochrome.ctb\n 44\n841.0\n 45\n594.0\n"
               " 70\n4\n 72\n1\n 73\n0\n 74\n1\n 75\n16\n142\n1.0\n143\n100.0\n"
               "  0\nPLOTSETTINGS\n  5\n2B\n100\nAcDbPlotSettings\n  1\nSmall\n"
               " 44\n210.0\n 45\n297.0\n 73\n1\n 75\n0\n"
               "  0\nPLOTSETTINGS\n  5\n2C\n100\nAcDbPlotSettings\n  1\n*None*\n"
               "  0\nLAYOUT\n  5\n2D\n100\nAcDbPlotSettings\n 44\n420.0\n 45\n297.0\n 70\n4\n"
               " 74\n1\n 75\n0\n100\nAcDbLayout\n  1\nSheet 1\n 70\n1\n 71\n1\n"
               "  0\nENDSEC\n  0\nEOF\n";
    }
    CHECK(f.host->run("PAGESETUP", {"Import", f.path("setups.dxf")}).finished);
    CHECK(f.host->logContains("3 page setups imported"));
    auto site = layout::findPageSetup(f.doc, "Site plan");
    REQUIRE(site);
    CHECK(site->settings.paperName == "A1");
    CHECK(site->settings.landscape);
    CHECK_FALSE(site->settings.fitToPaper);
    CHECK(site->settings.scaleN == doctest::Approx(100));
    CHECK(site->settings.centerOnPaper);
    CHECK(site->styleName == "monochrome.ctb");
    auto small = layout::findPageSetup(f.doc, "Small");
    REQUIRE(small);
    CHECK(small->settings.paperName == "A4");
    CHECK(small->settings.landscape);  // portrait paper turned a quarter
    CHECK(small->settings.fitToPaper);
    auto sheet = layout::findPageSetup(f.doc, "Sheet 1");
    REQUIRE(sheet);
    CHECK(sheet->settings.paperName == "A3");
    CHECK(sheet->settings.landscape);
}

TEST_CASE("PLOTSET / -PUBLISH plot a sheet list to a PDF, or say printing is unavailable") {
    Fixture f;
    REQUIRE(io::writeTbcad(f.doc, f.dir / "d.tbcad").ok());
    {
        std::ofstream list(f.dir / "list.txt");
        list << "d.tbcad\nd.tbcad\n";
    }
    CHECK(f.reg.find("-PUBLISH") == f.reg.find("PLOTSET"));
    const auto out = f.path("set.pdf");
    CHECK(f.host->run("-PUBLISH", {f.path("list.txt"), out}).finished);
    CHECK(f.host->logContains("Plotted 2 sheets"));
    CHECK(std::filesystem::exists(out));
    if (!print::available()) {  // never send a test job to a real printer
        CHECK(f.host->run("PLOTSET", {f.path("list.txt"), "Printer"}).finished);
        CHECK(f.host->logContains("Printing is only available"));
    }
}

TEST_CASE("PUBLISH opens the dialog - PLOTSTAMP sets the stamp - PDF layers can be left out") {
    Fixture f;
    DocHost h(f.doc, f.reg);
    CHECK(h.run("PUBLISH", {}).finished);
    REQUIRE(h.panels.size() == 1);
    CHECK(h.panels[0] == "publish");

    CHECK(f.host->run("PLOTSTAMP", {"Text", "%file% %layout%", "On"}).finished);
    CHECK(plot::plotStampOn(*f.host));
    CHECK(plot::plotStampText(*f.host) == "%file% %layout%");
    CHECK(f.host->run("PLOTSTAMP", {"Off"}).finished);
    CHECK_FALSE(plot::plotStampOn(*f.host));

    // The sheet knows the line's layer; dropLayers forgets it everywhere.
    auto sheet = plot::buildPlotSheet(f.doc, *f.host, plot::loadSettings(*f.host));
    REQUIRE(sheet.ok());
    CHECK_FALSE(sheet.value().layerNames.empty());
    exportlib::dropLayers(sheet.value());
    CHECK(sheet.value().layerNames.empty());
    for (const auto& prim : sheet.value().prims)
        if (const auto* path = std::get_if<exportlib::PlotPath>(&prim))
            CHECK(path->style.layer == 0);

    // EXPORTPDF and -PUBLISH write the layer list unless "include layer information" is off.
    CHECK(plot::pdfLayersOn(*f.host));
    CHECK(f.host->run("EXPORTPDF", {"", f.path("with.pdf")}).finished);
    CHECK(slurp(f.path("with.pdf")).find("/OCProperties") != std::string::npos);
    CHECK(f.host->run("EXPORTPDF", {"PdfLayers", "No", "", f.path("flat.pdf")}).finished);
    CHECK_FALSE(plot::pdfLayersOn(*f.host));
    CHECK(slurp(f.path("flat.pdf")).find("/OCProperties") == std::string::npos);
    REQUIRE(io::writeTbcad(f.doc, f.dir / "d.tbcad").ok());
    std::ofstream(f.dir / "list.tbdsd") << "d.tbcad\n";
    CHECK(f.host->run("-PUBLISH", {f.path("list.tbdsd"), f.path("set.pdf")}).finished);
    CHECK(slurp(f.path("set.pdf")).find("/OCProperties") == std::string::npos);
    plot::setPdfLayers(*f.host, true);
    CHECK(f.host->run("-PUBLISH", {f.path("list.tbdsd"), f.path("set2.pdf")}).finished);
    CHECK(slurp(f.path("set2.pdf")).find("/OCProperties") != std::string::npos);
}

TEST_CASE("the plot stamp adds text to the sheet and expands its tokens") {
    static const bool registered = (text::registerDisplay(), true);
    (void)registered;
    Fixture f;
    const auto settings = plot::loadSettings(*f.host);
    auto plain = plot::buildPlotSheet(f.doc, *f.host, settings);
    REQUIRE(plain.ok());
    plot::setPlotStamp(*f.host, true);
    plot::setPlotStampText(*f.host, "%paper% %layout% %scale%");
    auto stamped = plot::buildPlotSheet(f.doc, *f.host, settings);
    REQUIRE(stamped.ok());
    CHECK(stamped.value().prims.size() > plain.value().prims.size());
    CHECK(stamped.value().entityCount == plain.value().entityCount);
    const auto text = plot::expandStamp("%paper% | %layout% | %file% | %date%", *f.host,
                                        stamped.value(), settings);
    CHECK(text.rfind("A3 | Model | Untitled | 20", 0) == 0);
    plot::setPlotStamp(*f.host, false);
    CHECK(plot::buildPlotSheet(f.doc, *f.host, settings).value().prims.size() ==
          plain.value().prims.size());
    // The Stamp option of PLOT sets both.
    CHECK(f.host->run("EXPORTSVG", {"Stamp", "On", "%file% only", "", f.path("s.svg")}).finished);
    CHECK(plot::plotStampOn(*f.host));
    CHECK(plot::plotStampText(*f.host) == "%file% only");
}

TEST_CASE("a named style table is found next to the drawing - a missing one is reported") {
    Fixture f;
    DocHost h(f.doc, f.reg);
    h.file = (f.dir / "plan.tbcad").string();

    // Not there yet: the error says where it looked and plotting would use no style.
    plot::setPlotStyleName(h, "_EP-100.ctb");
    auto missing = plot::resolveStyleTable(h, "_EP-100.ctb");
    CHECK_FALSE(missing.ok());
    CHECK(plot::styleTableMissing(h));
    CHECK_FALSE(plot::loadSettings(h).styleTable);

    // A table saved beside the drawing is picked up by name, with or without the extension.
    plotstyle::PlotStyleTable t = *plotstyle::builtinTable("monochrome.ctb");
    t.name = "_EP-100";
    REQUIRE(plotstyle::saveFile(t, f.dir / "_EP-100.tbstyle").ok());
    plot::setPlotStyleName(h, "_EP-100");
    CHECK_FALSE(plot::styleTableMissing(h));
    CHECK(plot::loadSettings(h).styleTable);
    plot::setPlotStyleName(h, "_EP-100.tbstyle");
    CHECK_FALSE(plot::styleTableMissing(h));
}

TEST_CASE("plot style folders added by the user are searched") {
    Fixture f;
    DocHost h(f.doc, f.reg);
    const auto extra = f.dir / "styles_from_office";
    std::filesystem::create_directories(extra);
    plotstyle::PlotStyleTable t = *plotstyle::builtinTable("monochrome.ctb");
    t.name = "_OFFICE";
    REQUIRE(plotstyle::saveFile(t, extra / "_OFFICE.tbstyle").ok());
    CHECK_FALSE(plot::resolveStyleTable(h, "_OFFICE").ok());

    h.setSetting("plot.stylepaths", "nowhere;" + extra.string());
    CHECK(plot::resolveStyleTable(h, "_OFFICE").ok());
    CHECK(plot::userStyleFolders(h).size() == 2);

    // The PLOTSTYLE Paths command edits the same list.
    h.setSetting("plot.stylepaths", "");
    h.run("PLOTSTYLE", {"Paths", "Add", extra.string()});
    CHECK(plot::userStyleFolders(h).size() == 1);
    h.run("PLOTSTYLE", {"Paths", "Reset"});
    CHECK(plot::userStyleFolders(h).empty());
}

TEST_CASE("a style imported under another spelling is still found by the layout's name") {
    Fixture f;
    DocHost h(f.doc, f.reg);
    h.file = (f.dir / "plan.tbcad").string();
    plotstyle::PlotStyleTable t = *plotstyle::builtinTable("monochrome.ctb");
    t.name = "EP100";
    REQUIRE(plotstyle::saveFile(t, f.dir / "EP100.tbstyle").ok());
    CHECK(plot::resolveStyleTable(h, "_EP-100.ctb").ok());
    CHECK(plot::resolveStyleTable(h, "_ep-100").ok());
    CHECK_FALSE(plot::resolveStyleTable(h, "_EP-101.ctb").ok());
}

TEST_CASE("a style table is read once and again when its file changes") {
    Fixture f;
    DocHost h(f.doc, f.reg);
    h.file = (f.dir / "plan.tbcad").string();
    plotstyle::PlotStyleTable t = *plotstyle::builtinTable("monochrome.ctb");
    t.name = "Office";
    t.entries[1].screen = 40;
    const auto file = f.dir / "Office.tbstyle";
    REQUIRE(plotstyle::saveFile(t, file).ok());
    auto a = plot::resolveStyleTable(h, "Office.tbstyle");
    REQUIRE(a.ok());
    auto b = plot::resolveStyleTable(h, "Office.tbstyle");
    REQUIRE(b.ok());
    CHECK(a.value().get() == b.value().get());  // the same parsed table
    t.entries[1].screen = 70;
    REQUIRE(plotstyle::saveFile(t, file).ok());
    std::filesystem::last_write_time(
        file, std::filesystem::last_write_time(file) + std::chrono::seconds(2));
    auto c = plot::resolveStyleTable(h, "Office.tbstyle");
    REQUIRE(c.ok());
    CHECK(c.value()->entries[1].screen == 70);
}

TEST_CASE("PLOT asks before replacing a file that already exists") {
    Fixture f;
    const auto out = f.path("again.pdf");
    { std::ofstream(out) << "old"; }
    auto r = f.host->run("PLOT", {"Export", out, "No", cmd::ScriptItem::esc()});
    CHECK(r.finished);
    CHECK(std::filesystem::file_size(out) == 3);  // untouched
    f.host->run("PLOT", {"Export", out, "Yes"});
    CHECK(std::filesystem::file_size(out) > 3);
}

TEST_CASE("PLOT never stops to ask when the layout's style table cannot be found") {
    Fixture f;
    plot::setPlotStyleName(*f.host, "nowhere.ctb");
    auto out = f.path("m.pdf");
    f.host->clearLog();
    auto r = f.host->run("PLOT", {"Export", out});  // a script: the file name comes next
    CHECK(r.finished);
    CHECK(f.host->logContains("was not found"));
    CHECK(plot::plotStyleName(*f.host) == "nowhere.ctb");
    CHECK(std::filesystem::exists(out));
}

TEST_CASE("a plot style chosen after the layout overrides the layout's page setup") {
    Fixture f;
    {
        auto tx = f.doc.begin("layout");
        REQUIRE(layout::createLayout(tx, "Detail", "A3", true).ok());
        tx.commit();
    }
    CHECK(f.host->run("EXPORTSVG", {"Style", "none", "", f.path("x.svg")}).finished);
    CHECK(f.host->run("PAGESETUP", {"Save", "Plain"}).finished);
    CHECK(f.host->run("PAGESETUP", {"Assign", "Plain", "Detail"}).finished);
    CHECK(f.host
              ->run("EXPORTSVG",
                    {"Layout", "Detail", "Style", "monochrome.ctb", "Export", f.path("z.svg")})
              .finished);
    CHECK(plot::plotStyleName(*f.host) == "monochrome.ctb");
}

TEST_CASE("the PLOT menu sets the lineweight scale and the screening, and keeps them") {
    Fixture f;
    CHECK(f.host->run("EXPORTSVG", {"LWScale", "2", "Screening", "40", "", f.path("lw.svg")})
              .finished);
    const auto s = plot::loadSettings(*f.host);
    CHECK(s.lineweightScale == doctest::Approx(2));
    CHECK(s.screening == 40);
    CHECK(f.host->run("PAGESETUP", {"Save", "Light"}).finished);
    auto saved = layout::findPageSetup(f.doc, "light");
    REQUIRE(saved);
    CHECK(saved->settings.screening == 40);
    CHECK(saved->settings.lineweightScale == doctest::Approx(2));
}

TEST_CASE("COMPARE marks added, removed and changed objects") {
    Fixture f;  // the open drawing holds one line (0,0)-(10000,0)
    Document old;
    {
        auto tx = old.begin("old");
        Entity same;
        same.data = LineData{{0, 0}, {10000, 0}};
        REQUIRE(tx.add(same).ok());
        Entity moved;
        moved.data = LineData{{0, 500}, {1000, 500}};
        REQUIRE(tx.add(moved).ok());
        Entity gone;
        gone.data = CircleData{{5000, 5000}, 100};
        REQUIRE(tx.add(gone).ok());
        tx.commit();
    }
    {
        auto tx = f.doc.begin("new");
        Entity moved;
        moved.data = LineData{{0, 510}, {1000, 510}};
        REQUIRE(tx.add(moved).ok());
        Entity added;
        added.data = LineData{{20000, 0}, {20000, 100}};
        REQUIRE(tx.add(added).ok());
        tx.commit();
    }
    REQUIRE(io::writeTbcad(old, f.path("old.tbcad")).ok());
    auto r = f.host->run("COMPARE", {f.path("old.tbcad")});
    CHECK(r.finished);
    CHECK(f.host->logContains("1 added (green), 1 removed (red), 1 changed (yellow)"));
    CHECK(f.doc.layerByName("COMPARE-ADDED") != nullptr);
    CHECK(f.doc.layerByName("COMPARE-REMOVED") != nullptr);
    CHECK(f.doc.layerByName("COMPARE-CHANGED") != nullptr);
}
