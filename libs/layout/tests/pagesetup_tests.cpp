#include <doctest/doctest.h>

#include <functional>

#include "tbcad/io/tbcad.h"
#include "tbcad/layout/layout.h"
#include "tbcad/layout/pagesetup.h"

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

PageSetup sample() {
    PageSetup p;
    p.name = "A1 colour";
    p.settings.paperName = "A1";
    p.settings.landscape = false;
    p.settings.area = exportlib::PlotArea::Window;
    p.settings.window.expand({1, 2});
    p.settings.window.expand({300.5, 400.25});
    p.settings.fitToPaper = false;
    p.settings.scaleN = 50;
    p.settings.centerOnPaper = false;
    p.settings.offsetMm = {5, 7.5};
    p.settings.marginMm = 4;
    p.settings.lineweights = false;
    p.settings.color = exportlib::ColorMode::Greyscale;
    p.styleName = "grayscale.ctb";
    return p;
}

}  // namespace

TEST_CASE("page setups round trip through the document and its undo") {
    Document doc;
    run(doc, [](Transaction& tx) { return savePageSetup(tx, sample()); });
    auto all = pageSetups(doc);
    REQUIRE(all.size() == 1);
    const PageSetup& p = all[0];
    CHECK(p.name == "A1 colour");
    CHECK(p.settings.paperName == "A1");
    CHECK_FALSE(p.settings.landscape);
    CHECK(p.settings.area == exportlib::PlotArea::Window);
    CHECK(p.settings.window.min.x == doctest::Approx(1));
    CHECK(p.settings.window.max.y == doctest::Approx(400.25));
    CHECK_FALSE(p.settings.fitToPaper);
    CHECK(p.settings.scaleN == doctest::Approx(50));
    CHECK_FALSE(p.settings.centerOnPaper);
    CHECK(p.settings.offsetMm.y == doctest::Approx(7.5));
    CHECK(p.settings.marginMm == doctest::Approx(4));
    CHECK_FALSE(p.settings.lineweights);
    CHECK(p.settings.color == exportlib::ColorMode::Greyscale);
    CHECK(p.styleName == "grayscale.ctb");
    CHECK(findPageSetup(doc, "a1 COLOUR").has_value());

    // Saving under the same name replaces; names are unique ignoring case.
    PageSetup q = sample();
    q.name = "a1 colour";
    q.settings.scaleN = 20;
    auto tx = doc.begin("again");
    REQUIRE(savePageSetup(tx, q).ok());
    ChangeSet cs = tx.commit();
    REQUIRE(pageSetups(doc).size() == 1);
    CHECK(pageSetups(doc)[0].name == "A1 colour");
    CHECK(pageSetups(doc)[0].settings.scaleN == doctest::Approx(20));
    CHECK_FALSE(validatePageSetupName(doc, "A1 Colour").ok());
    CHECK_FALSE(validatePageSetupName(doc, "").ok());
    CHECK_FALSE(validatePageSetupName(doc, " x").ok());
    CHECK(validatePageSetupName(doc, "A1 Colour", "a1 colour").ok());

    doc.apply(cs, Direction::Undo);
    CHECK(pageSetups(doc).size() == 1);
    CHECK(pageSetups(doc)[0].settings.scaleN == doctest::Approx(50));
}

TEST_CASE("assigning a page setup gives the layout its paper; rename and delete follow") {
    Document doc;
    run(doc, [](Transaction& tx) { return createLayout(tx, "Plan", "A4", true); });
    run(doc, [](Transaction& tx) { return savePageSetup(tx, sample()); });
    auto bad = [&] {
        auto tx = doc.begin("x");
        return assignPageSetup(tx, "Plan", "Nope").ok();
    };
    CHECK_FALSE(bad());
    run(doc, [](Transaction& tx) { return assignPageSetup(tx, "Plan", "a1 colour"); });
    auto l = findLayout(doc, "Plan");
    REQUIRE(l);
    CHECK(l->pageSetup == "A1 colour");
    CHECK(l->paperName == "A1");
    CHECK_FALSE(l->landscape);
    CHECK(l->widthMm == doctest::Approx(594));
    CHECK(l->heightMm == doctest::Approx(841));
    REQUIRE(pageSetupOfLayout(doc, "Plan"));

    run(doc, [](Transaction& tx) { return renamePageSetup(tx, "A1 colour", "Big"); });
    CHECK(findLayout(doc, "Plan")->pageSetup == "Big");
    run(doc, [](Transaction& tx) { return assignModelPageSetup(tx, "big"); });
    CHECK(modelPageSetup(doc) == "Big");
    CHECK(pageSetupOfModel(doc).has_value());

    run(doc, [](Transaction& tx) { return deletePageSetup(tx, "Big"); });
    CHECK(pageSetups(doc).empty());
    CHECK(findLayout(doc, "Plan")->pageSetup.empty());
    CHECK(modelPageSetup(doc).empty());
    CHECK_FALSE(pageSetupOfLayout(doc, "Plan"));
}

TEST_CASE("page setups survive saving the drawing") {
    Document doc;
    run(doc, [](Transaction& tx) { return createLayout(tx, "Plan", "A3", true); });
    run(doc, [](Transaction& tx) { return savePageSetup(tx, sample()); });
    run(doc, [](Transaction& tx) { return assignPageSetup(tx, "Plan", "A1 colour"); });
    const auto bytes = io::writeTbcadToMemory(doc);
    REQUIRE(bytes.ok());
    io::IoReport report;
    auto loaded = io::readTbcad(bytes.value(), report);
    REQUIRE(loaded.ok());
    CHECK(pageSetups(*loaded.value()).size() == 1);
    CHECK(findLayout(*loaded.value(), "Plan")->pageSetup == "A1 colour");
}
