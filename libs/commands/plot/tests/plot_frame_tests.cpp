#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "tbcad/cmd/cmd.h"
#include "tbcad/layout/plotframes.h"
#include "tbcad/plot/plot.h"

using namespace tbcad;

namespace {

struct FrameFixture {
    Document doc;
    cmd::Registry reg;
    std::unique_ptr<cmd::ScriptHost> host;
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("tbcad_plot_frame_tests_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    EntityId border{};

    FrameFixture() {
        std::filesystem::create_directories(dir);
        plot::registerCommands(reg);
        auto tx = doc.begin("lines");
        Entity e;
        e.data = LineData{{0, 0}, {80000, 0}};
        REQUIRE(tx.add(e).ok());
        PolylineData pl;
        pl.v = {{{100000, 0}}, {{142000, 0}}, {{142000, 29700}}, {{100000, 29700}}};
        pl.closed = true;
        auto id = tx.add(Entity{{}, pl});
        REQUIRE(id.ok());
        border = id.value();
        tx.commit();
        host = std::make_unique<cmd::ScriptHost>(doc, reg);
        host->setSetting("files.filedia", "0");
    }
    ~FrameFixture() { std::filesystem::remove_all(dir); }
    std::string path(const char* name) const { return (dir / name).string(); }
};

int pdfPages(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    int n = 0;
    for (std::size_t i = s.find("/Type /Page"); i != std::string::npos;
         i = s.find("/Type /Page", i + 1))
        if (s.compare(i, 12, "/Type /Pages") != 0)
            ++n;
    return n;
}

}  // namespace

TEST_CASE("PLOTFRAME New places a sheet of paper at a scale, All plots every frame") {
    FrameFixture f;
    auto r = f.host->run("PLOTFRAME", {"New", "A3", "Landscape", "1:100", "0,0", ""});
    REQUIRE(r.finished);
    auto a = layout::findPlotFrame(f.doc, "Sheet 1");
    REQUIRE(a);
    CHECK(a->scaleN == 100);
    CHECK(a->window.width() == doctest::Approx((420 - 2 * 10) * 100.0));
    CHECK(a->window.height() == doctest::Approx((297 - 2 * 10) * 100.0));

    r = f.host->run("PLOTFRAME",
                    {"Window", "A1", "Portrait", "1:50", "0,0", "20000,30000", "Plan 2"});
    REQUIRE(r.finished);
    auto b = layout::findPlotFrame(f.doc, "Plan 2");
    REQUIRE(b);
    CHECK(b->paperName == "A1");
    CHECK_FALSE(b->landscape);
    CHECK(b->scaleN == 50);

    // Undo removes the second frame only.
    CHECK(f.host->undo());
    CHECK_FALSE(layout::findPlotFrame(f.doc, "Plan 2"));
    CHECK(layout::findPlotFrame(f.doc, "Sheet 1"));
    REQUIRE(
        f.host
            ->run("PLOTFRAME", {"Window", "A1", "Portrait", "1:50", "0,0", "20000,30000", "Plan 2"})
            .finished);

    const std::string out = f.path("all.pdf");
    r = f.host->run("PLOTFRAME", {"All", out});
    REQUIRE(r.finished);
    CHECK(f.host->logContains("Plotted 2 plot frames"));
    CHECK(pdfPages(out) == 2);

    const std::string one = f.path("one.pdf");
    REQUIRE(f.host->run("PLOTFRAME", {"Plot", "plan 2", one}).finished);
    CHECK(pdfPages(one) == 1);

    auto sheets = plot::buildFrameSheets(f.doc, *f.host, {"Sheet 1", "Plan 2"});
    REQUIRE(sheets);
    CHECK(sheets.value()[0].layout.paperPerUnit == doctest::Approx(1.0 / 100));
    CHECK(sheets.value()[1].layout.paperPerUnit == doctest::Approx(1.0 / 50));
    CHECK(sheets.value()[1].layout.annoScale == doctest::Approx(50));
    CHECK_FALSE(plot::buildFrameSheets(f.doc, *f.host, {"Nope"}));
}

TEST_CASE("PLOTFRAME Object makes one frame per selected border, Rename and Delete") {
    FrameFixture f;
    auto r = f.host->run("PLOTFRAME", {"Object", cmd::ScriptItem::select({f.border}), "", "A3",
                                       "Landscape", "1:100", ""});
    REQUIRE(r.finished);
    auto fr = layout::findPlotFrame(f.doc, "Sheet 1");
    REQUIRE(fr);
    CHECK(fr->window.min.x == doctest::Approx(100000));
    CHECK(fr->window.max.y == doctest::Approx(29700));
    REQUIRE(f.host->run("PLOTFRAME", {"Rename", "Sheet 1", "Ground floor"}).finished);
    CHECK(layout::findPlotFrame(f.doc, "ground floor"));
    REQUIRE(f.host->run("PLOTFRAME", {"Delete", "Ground floor"}).finished);
    CHECK(layout::plotFrames(f.doc).empty());
}

TEST_CASE("PLOT Area Frame plots with the frame's paper and scale") {
    FrameFixture f;
    REQUIRE(f.host->run("PLOTFRAME", {"New", "A2", "Landscape", "1:200", "0,0", ""}).finished);
    const std::string out = f.path("frame.pdf");
    auto r = f.host->run("PLOT", {"Area", "Frame", "Sheet 1", "", out});
    REQUIRE(r.finished);
    CHECK(f.host->logContains("scale 1:200"));
    CHECK(f.host->logContains("A2"));
}

TEST_CASE("FRAMESET: a named set of sheets, copied, changed and plotted on its own") {
    FrameFixture f;
    for (int i = 0; i < 4; ++i)
        REQUIRE(f.host
                    ->run("PLOTFRAME",
                          {"New", "A3", "Landscape", "1:100", std::to_string(i * 50000) + ",0", ""})
                    .finished);
    REQUIRE(f.host->run("FRAMESET", {"New", "Issue 1", "1,3-4"}).finished);
    auto set = layout::findPlotSet(f.doc, "Issue 1");
    REQUIRE(set);
    CHECK(set->frames == std::vector<std::string>{"Sheet 1", "Sheet 3", "Sheet 4"});

    REQUIRE(f.host->run("FRAMESET", {"Copy", "Issue 1", "Issue 2"}).finished);
    REQUIRE(f.host->run("FRAMESET", {"Remove", "Issue 2", "3"}).finished);
    REQUIRE(f.host->run("FRAMESET", {"Add", "Issue 2", "Sheet 2"}).finished);
    CHECK(layout::findPlotSet(f.doc, "Issue 2")->frames ==
          std::vector<std::string>{"Sheet 1", "Sheet 4", "Sheet 2"});
    CHECK(f.host->undo());  // the Add
    CHECK(layout::findPlotSet(f.doc, "Issue 2")->frames.size() == 2);

    // New with an existing name replaces its sheets (the panel saves checked sheets so).
    REQUIRE(f.host->run("FRAMESET", {"New", "Issue 2", "2"}).finished);
    CHECK(layout::findPlotSet(f.doc, "Issue 2")->frames == std::vector<std::string>{"Sheet 2"});

    const std::string out = f.path("issue1.pdf");
    REQUIRE(f.host->run("FRAMESET", {"Plot", "Issue 1", out}).finished);
    CHECK(f.host->logContains("Plotted 3 plot frames"));
    CHECK(pdfPages(out) == 3);

    // PLOTFRAME Plot takes numbers too.
    const std::string two = f.path("two.pdf");
    REQUIRE(f.host->run("PLOTFRAME", {"Plot", "2,4", two}).finished);
    CHECK(pdfPages(two) == 2);

    REQUIRE(f.host->run("FRAMESET", {"Rename", "Issue 2", "Tender"}).finished);
    REQUIRE(f.host->run("FRAMESET", {"Delete", "Issue 1"}).finished);
    REQUIRE(layout::plotSets(f.doc).size() == 1);
    CHECK(layout::plotSets(f.doc)[0].name == "Tender");
}
