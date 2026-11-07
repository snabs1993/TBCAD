#include <doctest/doctest.h>

#include <functional>

#include "tbcad/layout/layout.h"
#include "tbcad/layout/paper_size.h"
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

EntityId addText(Document& doc, Vec2 at, double height, bool annotative = false,
                 BlockId owner = {}) {
    TextData t;
    t.position = at;
    t.height = height;
    t.utf8 = "NOTE";
    Entity e;
    e.common.annotative = annotative;
    e.data = Indirect<TextData>(std::move(t));
    EntityId id{};
    run(doc, [&](Transaction& tx) {
        auto r = tx.add(std::move(e), owner);
        if (!r)
            return Result<void>(r.error());
        id = r.value();
        return Result<void>{};
    });
    return id;
}

}  // namespace

TEST_CASE("text height on paper follows the plot frame that holds it") {
    Document doc;
    PlotFrame f;
    f.name = "Plan";
    f.window.expand({0, 0});
    f.window.expand({84100, 59400});
    f.paperName = "A1";
    f.scaleN = 100;
    run(doc, [&](Transaction& tx) { return savePlotFrame(tx, f); });

    const EntityId small = addText(doc, {1000, 1000}, 120);
    const EntityId good = addText(doc, {2000, 1000}, 250);
    const auto pl = paperPlaceOf(doc, small);
    REQUIRE(pl);
    CHECK(pl->kind == PaperPlace::Kind::Frame);
    CHECK(pl->scaleN == 100);
    CHECK(describePaperPlace(*pl) == "1:100, frame Plan (A1)");
    CHECK(*paperTextHeightMm(doc, *doc.find(small), *pl) == doctest::Approx(1.2));
    CHECK(*paperTextHeightMm(doc, *doc.find(good), *pl) == doctest::Approx(2.5));

    // Annotative text keeps its paper height whatever the frame scale.
    const EntityId anno = addText(doc, {3000, 1000}, 2.5, true);
    CHECK(*paperTextHeightMm(doc, *doc.find(anno), *paperPlaceOf(doc, anno)) ==
          doctest::Approx(2.5));

    const auto s = paperSizeSummary(doc, {small, good, anno});
    CHECK(s.count == 3);
    CHECK(formatPaperMm(s.minMm, s.maxMm) == "1.2 - 2.5 mm");
    REQUIRE(s.place);
    CHECK(s.place->name == "Plan");
}

TEST_CASE("outside frames: viewports, then zones, then the annotation scale") {
    Document doc;
    run(doc, [](Transaction& tx) { return createLayout(tx, "S1", "A3", true); });
    BBox2 rect;
    rect.expand({10, 10});
    rect.expand({110, 90});  // 100 x 80 mm on the sheet
    run(doc, [&](Transaction& tx) {
        auto r = addViewport(tx, "S1", rect, {0, 0}, 50);
        return r ? Result<void>{} : Result<void>(r.error());
    });
    const EntityId inVp = addText(doc, {1000, 1000}, 125);  // view is +-2500 x +-2000
    auto pl = paperPlaceOf(doc, inVp);
    REQUIRE(pl);
    CHECK(pl->kind == PaperPlace::Kind::Viewport);
    CHECK(describePaperPlace(*pl) == "1:50, S1 viewport 1 (A3)");
    CHECK(*paperTextHeightMm(doc, *doc.find(inVp), *pl) == doctest::Approx(2.5));

    run(doc, [](Transaction& tx) {
        return tx.modifyHeader([](Header& h) {
            AnnotationZone z;
            z.name = "Detail";
            z.scale = 20;
            z.box.expand({10000, 0});
            z.box.expand({12000, 2000});
            setAnnotationZones(h, {z});
            h.custom[kAnnoScaleKey] = "200";
        });
    });
    const EntityId inZone = addText(doc, {11000, 1000}, 50);
    pl = paperPlaceOf(doc, inZone);
    REQUIRE(pl);
    CHECK(pl->kind == PaperPlace::Kind::Zone);
    CHECK(*paperTextHeightMm(doc, *doc.find(inZone), *pl) == doctest::Approx(2.5));

    const EntityId far = addText(doc, {90000, 90000}, 500);
    pl = paperPlaceOf(doc, far);
    REQUIRE(pl);
    CHECK(pl->kind == PaperPlace::Kind::AnnotationScale);
    CHECK(describePaperPlace(*pl) == "1:200, annotation scale");
    CHECK(*paperTextHeightMm(doc, *doc.find(far), *pl) == doctest::Approx(2.5));

    // Text on the sheet itself is already in paper millimetres.
    const BlockId content = findLayout(doc, "S1")->content;
    const EntityId onSheet = addText(doc, {20, 20}, 3.5, false, content);
    pl = paperPlaceOf(doc, onSheet);
    REQUIRE(pl);
    CHECK(pl->kind == PaperPlace::Kind::Sheet);
    CHECK(describePaperPlace(*pl) == "sheet S1 (A3)");
    CHECK(*paperTextHeightMm(doc, *doc.find(onSheet), *pl) == doctest::Approx(3.5));
}

TEST_CASE("dimension text on paper uses the style height times its overall scale") {
    Document doc;
    run(doc, [](Transaction& tx) {
        return tx.modifyHeader([](Header& h) { h.custom[kAnnoScaleKey] = "100"; });
    });
    // STANDARD: 2.5 high. Overall scale 50 = made for 1:50, so 1.25 mm at 1:100.
    run(doc, [&](Transaction& tx) {
        return tx.modifyDimStyle(doc.header().currentDimStyle,
                                 [](DimStyle& st) { st.overallScale = 50; });
    });
    DimensionData d;
    d.defPts = {Vec2{0, 0}, Vec2{1000, 0}, Vec2{0, 500}, Vec2{}};
    Entity e;
    e.data = Indirect<DimensionData>(std::move(d));
    EntityId id{};
    run(doc, [&](Transaction& tx) {
        auto r = tx.add(std::move(e));
        if (!r)
            return Result<void>(r.error());
        id = r.value();
        return Result<void>{};
    });
    const auto pl = paperPlaceOf(doc, id);
    REQUIRE(pl);
    CHECK(*paperTextHeightMm(doc, *doc.find(id), *pl) == doctest::Approx(1.25));
    CHECK(formatPaperMm(1.25, 1.25) == "1.25 mm");
}

TEST_CASE("paper check finds text left at an old scale and fits it in one undo") {
    Document doc;
    PlotFrame f;
    f.name = "Plan";
    f.window.expand({0, 0});
    f.window.expand({84100, 59400});
    f.paperName = "A1";
    f.scaleN = 100;
    run(doc, [&](Transaction& tx) { return savePlotFrame(tx, f); });
    const EntityId ok = addText(doc, {1000, 1000}, 250);   // 2.5 mm
    const EntityId old = addText(doc, {2000, 1000}, 125);  // 1.25 mm, 2.5 mm at 1:50
    const EntityId tiny = addText(doc, {3000, 1000}, 60);  // 0.6 mm, no scale fits
    const EntityId odd = addText(doc, {4000, 1000}, 520);  // 5.2 mm

    const auto items = paperCheck(doc);
    REQUIRE(items.size() == 4);
    auto find = [&](EntityId id) {
        for (const auto& it : items)
            if (it.id == id)
                return it;
        FAIL("missing");
        return PaperCheckItem{};
    };
    CHECK(find(ok).issue == PaperIssue::Ok);
    CHECK(find(old).issue == PaperIssue::OtherScale);
    CHECK(find(old).madeForN == 50);
    CHECK(find(old).suggestMm == doctest::Approx(2.5));
    CHECK(find(tiny).issue == PaperIssue::TooSmall);
    CHECK(find(tiny).suggestMm == doctest::Approx(1.8));
    CHECK(find(odd).issue == PaperIssue::OffStandard);
    CHECK(find(odd).suggestMm == doctest::Approx(5));
    CHECK(std::string(paperIssueLabel(PaperIssue::OtherScale)) == "Made for another scale");

    // Only the selected ones.
    CHECK(paperCheck(doc, {old}).size() == 1);

    ChangeSet cs;
    {
        auto tx = doc.begin("fit");
        auto r = fitToPaper(tx, items);
        REQUIRE(r.ok());
        CHECK(r.value() == 3);
        cs = tx.commit();
    }
    CHECK(entityAs<TextData>(*doc.find(old))->height == doctest::Approx(250));
    CHECK(entityAs<TextData>(*doc.find(tiny))->height == doctest::Approx(180));
    CHECK(entityAs<TextData>(*doc.find(odd))->height == doctest::Approx(500));
    for (const auto& it : paperCheck(doc))
        CHECK(it.issue == PaperIssue::Ok);
    doc.apply(cs, Direction::Undo);
    CHECK(entityAs<TextData>(*doc.find(old))->height == doctest::Approx(125));
    CHECK(entityAs<TextData>(*doc.find(tiny))->height == doctest::Approx(60));
}

TEST_CASE("fitting a dimension gives it an override style at the frame scale") {
    Document doc;
    run(doc, [](Transaction& tx) {
        return tx.modifyHeader([](Header& h) { h.custom[kAnnoScaleKey] = "100"; });
    });
    DimensionData d;
    d.defPts = {Vec2{0, 0}, Vec2{1000, 0}, Vec2{0, 500}, Vec2{}};
    Entity e;
    e.data = Indirect<DimensionData>(std::move(d));
    EntityId id{};
    run(doc, [&](Transaction& tx) {
        auto r = tx.add(std::move(e));
        if (!r)
            return Result<void>(r.error());
        id = r.value();
        return Result<void>{};
    });
    auto items = paperCheck(doc);
    REQUIRE(items.size() == 1);
    CHECK(items[0].issue == PaperIssue::OtherScale);  // 2.5 high: made for 1:1
    run(doc, [&](Transaction& tx) {
        auto r = fitToPaper(tx, items);
        return r ? Result<void>{} : Result<void>(r.error());
    });
    const auto pl = paperPlaceOf(doc, id);
    CHECK(*paperTextHeightMm(doc, *doc.find(id), *pl) == doctest::Approx(2.5));
    CHECK(entityAs<DimensionData>(*doc.find(id))->style != doc.header().currentDimStyle);
}
