#include <doctest/doctest.h>

#include "tbcad/drafting/viewport_snap.h"
#include "test_support.h"

using namespace tbcad;
using namespace tbcad::drafting;
using namespace tbcad::drafting::test;

namespace {

// A layout "A" whose one viewport (sheet rectangle 20..220 x 20..120, scale 1:50) shows the model
// around (1000, 500).
struct Sheet : World {
    layout::Layout lay;
    int vp = 0;
    Sheet() {
        auto tx = doc.begin("layout");
        REQUIRE(layout::createLayout(tx, "A", "A3").ok());
        auto id = layout::addViewport(tx, "A", BBox2{{20, 20}, {220, 120}}, {1000, 500}, 50);
        REQUIRE(id.ok());
        vp = id.value();
        tx.commit();
        lay = *layout::findLayout(doc, "A");
    }
    SnapRequest request(Vec2 sheetCursor, double ap = 3) {
        SnapRequest rq;
        rq.cursor = sheetCursor;
        rq.apertureMm = ap;
        rq.modes = maskOf(SnapMode::Endpoint) | maskOf(SnapMode::Midpoint);
        return rq;
    }
};

}  // namespace

TEST_CASE("sheet and model points map through the viewport's centre and scale") {
    Sheet s;
    const layout::Viewport& v = s.lay.viewports.front();
    CHECK(near(sheetFromModel(v, {1000, 500}), {120, 70}));
    CHECK(near(sheetFromModel(v, {1500, 500}), {130, 70}));
    CHECK(near(modelFromSheet(v, {130, 80}), {1500, 1000}));
    const Vec2 p{1234.5, 678.9};
    CHECK(near(modelFromSheet(v, sheetFromModel(v, p)), p));
}

TEST_CASE("object snap on a sheet reaches model geometry through the viewport") {
    Sheet s;
    s.line({1000, 500}, {2000, 500});  // on the sheet: (120,70) to (140,70)
    // The cursor is near the line's end at (140, 70) on the sheet.
    auto found = findViewportSnapCandidates(s.doc, s.idx, s.lay, s.request({139, 71}));
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().mode == SnapMode::Endpoint);
    CHECK(near(found.front().point, {140, 70}));
    CHECK(found.front().distance == doctest::Approx(std::hypot(1.0, 1.0)));
    // Midpoint at (130, 70).
    found = findViewportSnapCandidates(s.doc, s.idx, s.lay, s.request({130.5, 69}));
    REQUIRE_FALSE(found.empty());
    CHECK(found.front().mode == SnapMode::Midpoint);
    CHECK(near(found.front().point, {130, 70}));
    // Away from the line nothing snaps.
    CHECK(findViewportSnapCandidates(s.doc, s.idx, s.lay, s.request({160, 100})).empty());
}

TEST_CASE("the viewport's scale decides the aperture in model units") {
    Sheet s;
    s.line({1000, 500}, {2000, 500});
    // 3 mm on the sheet is 150 model units: 100 sheet-mm... a cursor 2 mm (sheet) from the end
    // snaps, one 4 mm away does not.
    CHECK_FALSE(findViewportSnapCandidates(s.doc, s.idx, s.lay, s.request({142, 70})).empty());
    CHECK(findViewportSnapCandidates(s.doc, s.idx, s.lay, s.request({144, 70})).empty());
}

TEST_CASE("nothing snaps outside the viewport, in a viewport that is off, or on a frozen layer") {
    Sheet s;
    const EntityId l = s.line({1000, 500}, {2000, 500});
    // The cursor on the sheet outside the viewport rectangle.
    CHECK(findViewportSnapCandidates(s.doc, s.idx, s.lay, s.request({10, 10})).empty());
    {
        auto tx = s.doc.begin("off");
        REQUIRE(layout::setViewportOn(tx, "A", s.vp, false).ok());
        tx.commit();
    }
    CHECK(findViewportSnapCandidates(s.doc, s.idx, *layout::findLayout(s.doc, "A"),
                                     s.request({139, 71}))
              .empty());
    {
        auto tx = s.doc.begin("on");
        REQUIRE(layout::setViewportOn(tx, "A", s.vp, true).ok());
        REQUIRE(
            layout::setViewportLayerFrozen(tx, "A", s.vp, s.doc.find(l)->common.layer, true).ok());
        tx.commit();
    }
    CHECK(findViewportSnapCandidates(s.doc, s.idx, *layout::findLayout(s.doc, "A"),
                                     s.request({139, 71}))
              .empty());
}

TEST_CASE("a clipped viewport shows only the part inside its outline") {
    Sheet s;
    s.line({900, 400}, {1400, 400});  // sheet (118,68) .. (128,68)
    {
        auto tx = s.doc.begin("clip");
        // A triangle with the same bounding box as the rectangle (so the view does not move):
        // (118,68) is inside, (128,68) is beyond the slanted side.
        REQUIRE(layout::setViewportClip(tx, "A", s.vp, {{20, 20}, {220, 20}, {20, 120}}).ok());
        tx.commit();
    }
    const auto lay = *layout::findLayout(s.doc, "A");
    CHECK(findViewportSnapCandidates(s.doc, s.idx, lay, s.request({128, 68.5})).empty());
    auto found = findViewportSnapCandidates(s.doc, s.idx, lay, s.request({118.5, 68.5}));
    REQUIRE_FALSE(found.empty());
    CHECK(near(found.front().point, {118, 68}));
}

TEST_CASE("the object snap filter snaps to the model through a viewport while a layout is shown") {
    Sheet s;
    s.line({1000, 500}, {2000, 500});  // sheet (120,70) .. (140,70)
    s.set(keys::osnap, "1");
    s.set(keys::osnapModes, "endpoint");
    // In model space (no layout shown) the cursor is not near anything.
    CHECK(near(s.hover({139, 71}), {139, 71}));
    s.host.showLayout("A", &s.idx);
    cmd::FilterFeedback fb;
    CHECK(near(s.hover({139, 71}, std::nullopt, 1.0, &fb), {140, 70}));
    CHECK(fb.marker == "endpoint");
}
