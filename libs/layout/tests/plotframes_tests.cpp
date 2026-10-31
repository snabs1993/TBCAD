#include <doctest/doctest.h>

#include <functional>

#include "tbcad/layout/plotframes.h"

using namespace tbcad;
using namespace tbcad::layout;

namespace {

void run(Document& doc, const std::function<Result<void>(Transaction&)>& fn) {
    auto tx = doc.begin("test");
    auto r = fn(tx);
    if (!r.ok())
        FAIL(r.error().message);
    tx.commit();
}

PlotFrame frame(std::string name, double x) {
    PlotFrame f;
    f.name = std::move(name);
    f.window.expand({x, 0.25});
    f.window.expand({x + 40000.5, 27700});
    f.paperName = "A1";
    f.landscape = false;
    f.scaleN = 50;
    f.pageSetup = "Colour";
    return f;
}

}  // namespace

TEST_CASE("plot frames are saved in the drawing in order and round trip") {
    Document doc;
    CHECK(nextPlotFrameName(doc) == "Sheet 1");
    run(doc, [](Transaction& tx) { return savePlotFrame(tx, frame("Sheet 1", 0)); });
    run(doc, [](Transaction& tx) { return savePlotFrame(tx, frame("Roof", 50000)); });
    CHECK(nextPlotFrameName(doc) == "Sheet 2");
    auto all = plotFrames(doc);
    REQUIRE(all.size() == 2);
    const PlotFrame want = frame("Sheet 1", 0);
    CHECK(all[0].name == want.name);
    CHECK(all[0].window.min.x == want.window.min.x);
    CHECK(all[0].window.min.y == want.window.min.y);
    CHECK(all[0].window.max.x == want.window.max.x);
    CHECK(all[0].window.max.y == want.window.max.y);
    CHECK(all[0].paperName == "A1");
    CHECK_FALSE(all[0].landscape);
    CHECK(all[0].scaleN == 50);
    CHECK(all[0].pageSetup == "Colour");
    CHECK(all[1].name == "Roof");
    CHECK_FALSE(validatePlotFrameName(doc, "roof").ok());
    CHECK_FALSE(validatePlotFrameName(doc, "a,b").ok());
    CHECK(validatePlotFrameName(doc, "roof", "Roof").ok());

    // Replacing keeps the position; rename and delete.
    PlotFrame changed = frame("sheet 1", 0);
    changed.scaleN = 20;
    run(doc, [&](Transaction& tx) { return savePlotFrame(tx, changed); });
    all = plotFrames(doc);
    CHECK(all[0].name == "Sheet 1");
    CHECK(all[0].scaleN == 20);
    run(doc, [](Transaction& tx) { return renamePlotFrame(tx, "roof", "Roof plan"); });
    CHECK(findPlotFrame(doc, "ROOF PLAN"));
    run(doc, [](Transaction& tx) { return deletePlotFrame(tx, "Sheet 1"); });
    CHECK(plotFrames(doc).size() == 1);
}

TEST_CASE("frameBoxForPaper and frameSettings") {
    const BBox2 b = frameBoxForPaper("A3", true, 10, 100, {1000, 2000});
    CHECK(b.min.x == doctest::Approx(1000));
    CHECK(b.width() == doctest::Approx(40000));
    CHECK(b.height() == doctest::Approx(27700));
    CHECK(frameBoxForPaper("A3", false, 10, 100, {}).width() == doctest::Approx(27700));
    CHECK(frameBoxForPaper("Nope", true, 10, 100, {}).empty());

    Document doc;
    exportlib::PlotSettings base;
    base.marginMm = 5;
    base.centerOnPaper = false;
    PlotFrame f = frame("A", 0);
    f.pageSetup.clear();
    const auto s = frameSettings(doc, base, f);
    CHECK(s.area == exportlib::PlotArea::Window);
    CHECK(s.window.min.x == doctest::Approx(0));
    CHECK(s.paperName == "A1");
    CHECK_FALSE(s.landscape);
    CHECK_FALSE(s.fitToPaper);
    CHECK(s.scaleN == 50);
    CHECK(s.annoScale == 50);
    CHECK(s.centerOnPaper);
    CHECK(s.marginMm == 5);  // other options come from the base settings
}
