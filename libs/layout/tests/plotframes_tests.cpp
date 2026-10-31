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

TEST_CASE("plot sets: numbers and ranges, save, copy, add, remove, follow frame renames") {
    Document doc;
    for (int i = 1; i <= 10; ++i)
        run(doc, [&](Transaction& tx) {
            return savePlotFrame(tx, frame("Sheet " + std::to_string(i), i * 50000.0));
        });
    auto list = resolveFrameList(doc, "1, 5,7-9 , Sheet 10");
    REQUIRE(list);
    CHECK(list.value() == std::vector<std::string>{"Sheet 1", "Sheet 5", "Sheet 7", "Sheet 8",
                                                   "Sheet 9", "Sheet 10"});
    CHECK(resolveFrameList(doc, "3-1").value() ==
          std::vector<std::string>{"Sheet 3", "Sheet 2", "Sheet 1"});
    CHECK_FALSE(resolveFrameList(doc, "11"));
    CHECK_FALSE(resolveFrameList(doc, "Roof"));

    CHECK(nextPlotSetName(doc) == "Set 1");
    run(doc, [](Transaction& tx) {
        return savePlotSet(tx, {"Issue 1", {"Sheet 1", "sheet 5", "Sheet 7", "Sheet 1"}});
    });
    auto s = findPlotSet(doc, "issue 1");
    REQUIRE(s);
    CHECK(s->frames == std::vector<std::string>{"Sheet 1", "Sheet 5", "Sheet 7"});
    CHECK_FALSE(validatePlotSetName(doc, "ISSUE 1").ok());

    run(doc, [](Transaction& tx) { return copyPlotSet(tx, "Issue 1", "Issue 2"); });
    run(doc, [](Transaction& tx) { return addToPlotSet(tx, "Issue 2", {"Sheet 9", "Sheet 5"}); });
    run(doc, [](Transaction& tx) { return removeFromPlotSet(tx, "Issue 2", {"sheet 1"}); });
    CHECK(findPlotSet(doc, "Issue 2")->frames ==
          std::vector<std::string>{"Sheet 5", "Sheet 7", "Sheet 9"});
    CHECK(findPlotSet(doc, "Issue 1")->frames.size() == 3);

    // Frames renamed or deleted follow into every set.
    run(doc, [](Transaction& tx) { return renamePlotFrame(tx, "Sheet 5", "Roof"); });
    run(doc, [](Transaction& tx) { return deletePlotFrame(tx, "Sheet 7"); });
    CHECK(findPlotSet(doc, "Issue 1")->frames == std::vector<std::string>{"Sheet 1", "Roof"});
    CHECK(findPlotSet(doc, "Issue 2")->frames == std::vector<std::string>{"Roof", "Sheet 9"});

    run(doc, [](Transaction& tx) { return renamePlotSet(tx, "Issue 2", "Tender"); });
    run(doc, [](Transaction& tx) { return deletePlotSet(tx, "Issue 1"); });
    REQUIRE(plotSets(doc).size() == 1);
    CHECK(plotSets(doc)[0].name == "Tender");
    auto tx = doc.begin("bad");
    CHECK_FALSE(savePlotSet(tx, {"X", {"Nope"}}).ok());
}
